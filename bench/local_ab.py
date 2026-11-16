"""Local A/B driver on macOS: official JSONL transport, no Docker.

Usage: python3 bench/local_ab.py <binary> <rows> <queries> <rounds> <out.json> [--index]

Reuses bench/vendor protocol_check transport with command mode, generates the same
seed/parameters as bench/run_jsonl_baseline.py, verifies every answer against the
independent C++ oracle, and records write/query wall time plus child max RSS.
"""
import json, math, os, random, resource, statistics, struct, subprocess, sys, threading, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'bench/vendor/seekdb-round2-f4d4e26/protocol_check'))
from _transport import Session

def main():
    binary = Path(sys.argv[1]).resolve()
    N, Q, R = int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
    out = Path(sys.argv[5])
    index = '--index' in sys.argv
    work = out.parent / (out.stem + '-work')
    work.mkdir(parents=True, exist_ok=True)
    oracle = ROOT / 'build/oracle'
    assert oracle.exists(), 'prebuild build/oracle with: g++ -O2 -std=c++17 bench/jsonl_oracle.cpp -o build/oracle'

    rng = random.Random(20261110)
    mode = os.environ.get('MVDB_DATA', 'grid')
    def make():
        if mode == 'bits':      # arbitrary finite float32 magnitudes
            row = []
            while len(row) < 128:
                v = struct.unpack('<f', struct.pack('<I', rng.getrandbits(32)))[0]
                if v == v and abs(v) != float('inf'):
                    row.append(v)
            return row
        if mode == 'uniform':   # wide, smooth magnitudes
            return [rng.uniform(-1e4, 1e4) for _ in range(128)]
        return [rng.randrange(-2048, 2049) / 128 for _ in range(128)]
    vectors = [make() for _ in range(N)]
    queries = [make() for _ in range(Q)]
    order = list(range(N)); rng.shuffle(order)
    body = lambda i: 'record-' + str(i) + '-' + chr(97 + i % 26) * 256
    lit = lambda v: json.dumps(v, separators=(',', ':'))
    batch = int(os.environ.get('MVDB_BATCH', '100'))
    inserts = ['INSERT INTO t VALUES ' + ','.join(
        "(%d,%d,'%s','%s')" % (i, i % 10, lit(vectors[i]), body(i))
        for i in order[a:a + batch]) + ';' for a in range(0, N, batch)]
    sqls = ["SELECT id,L2_DISTANCE(v,'%s') AS d,body FROM t%s ORDER BY d LIMIT 10;" % (
        lit(v), (' WHERE g=' + str((i // 2) % 10)) if i % 2 else '') for i, v in enumerate(queries)]

    inp = work / 'oracle-input.bin'
    with inp.open('wb') as f:
        f.write(struct.pack('<III', N, Q, 128))
        for v in vectors + queries:
            f.write(struct.pack('<128f', *v))
    truth = [[(int(p.split(':')[0]), float(p.split(':')[1])) for p in line.split()]
             for line in subprocess.run([str(oracle), str(inp)], capture_output=True, check=True,
                                        text=True).stdout.splitlines()]

    config = json.loads((ROOT / 'bench/vendor/seekdb-round2-f4d4e26/protocol_check/config.json').read_text())
    config.update(request_timeout=300, case_timeout=7200)
    wrap = os.environ.get('MVDB_WRAP')
    command = ([wrap] if wrap else []) + [str(binary), '--data-dir', '{data_dir}']
    cgroup = os.environ.get('MVDB_CGROUP')
    rounds = []
    for r in range(R):
        tag = 'r%d-%s' % (r + 1, 'indexed' if index else 'unindexed')
        log = work / (tag + '-stderr.log')
        s = Session(config, log, command=command)
        rec = {'round': r + 1, 'indexed': index}
        # A fresh cgroup per round: memory.peak is a high-water mark and resetting
        # it by writing 0 is unsupported on older kernels.
        cgroup_dir = None
        if cgroup:
            cgroup_dir = Path(cgroup) / ('run-%d-%d' % (os.getpid(), r))
            cgroup_dir.mkdir(parents=True, exist_ok=True)
            os.environ['MVDB_CGROUP'] = str(cgroup_dir)
        io_before = cgroup_io(str(cgroup_dir)) if cgroup_dir else {}
        try:
            s.start()
            peak = {'bytes': 0}
            _, rec['create_ms'] = timed(s, 'CREATE TABLE t(id BIGINT PRIMARY KEY,g BIGINT,v VECTOR(128),body VARCHAR(512));')
            writes = []
            t0 = time.perf_counter()
            for sql in inserts:
                _, ms = timed(s, sql); writes.append(ms)
            rec['write_ms'] = (time.perf_counter() - t0) * 1000
            rec['write_request_ms'] = writes
            rec['index_build_ms'] = None
            if index:
                _, rec['index_build_ms'] = timed(s, 'CREATE VECTOR INDEX tv ON t(v);')
            rec['warmup_ms'] = []
            for j in range(10):
                resp, ms = timed(s, sqls[j % Q]); verify(resp, truth, j % Q); rec['warmup_ms'].append(ms)
            rec['query_ms'] = []
            for j in range(Q):
                resp, ms = timed(s, sqls[j % Q]); verify(resp, truth, j % Q); rec['query_ms'].append(ms)
            rec['query_total_ms'] = sum(rec['query_ms'])
            rec['data_bytes'] = sum(p.stat().st_size for p in Path(s.temp.name).rglob('*') if p.is_file())
            rec.update(proc_metrics(s.proc.pid))
            rec['db_peak_rss_bytes'] = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
            s.shutdown()
            if cgroup_dir:
                rec['cg_memory_peak_kb'] = read_int(cgroup_dir / 'memory.peak') // 1024
                io_after = cgroup_io(str(cgroup_dir))
                rec['cg_bytes'] = {k: io_after[k] - io_before.get(k, 0) for k in io_after}
        finally:
            s.close()
        rec['file_bytes'] = rec.get('data_bytes', 0)
        rounds.append(rec)
        print(json.dumps({k: rec[k] for k in ('round', 'indexed', 'write_ms', 'query_total_ms')}))
    ru = resource.getrusage(resource.RUSAGE_CHILDREN)
    summary = {
        'binary': str(binary), 'rows': N, 'queries': Q, 'rounds': R, 'indexed': index,
        'write_ms': [r['write_ms'] for r in rounds],
        'query_total_ms': [r['query_total_ms'] for r in rounds],
        'query_median_ms': [statistics.median(r['query_ms']) for r in rounds],
        'db_peak_rss_bytes': [r.get('db_peak_rss_bytes', 0) for r in rounds],
        'vmhwm_kb': [r.get('vmhwm_kb', 0) for r in rounds],
        'io_write_bytes': [r.get('io_write_bytes', 0) for r in rounds],
        'io_read_bytes': [r.get('io_read_bytes', 0) for r in rounds],
        'cpu_ms': [r.get('cpu_ms', 0) for r in rounds],
        'rounds_raw': rounds,
    }
    out.write_text(json.dumps(summary, indent=2))
    print(json.dumps({k: summary[k] for k in ('write_ms', 'query_total_ms', 'query_median_ms', 'db_peak_rss_bytes')}))

def read_int(path):
    try:
        return int(path.read_text().strip() or 0)
    except OSError:
        return 0

def cgroup_io(cgroup):
    total = {}
    try:
        for line in Path(cgroup, 'io.stat').read_text().splitlines():
            name = line.split()[0]
            values = dict(item.split('=') for item in line.split()[1:])
            total[name] = int(values.get('rbytes', 0)) + int(values.get('wbytes', 0))
    except OSError:
        pass
    return total

def proc_metrics(pid):
    out = {}
    try:
        for line in Path('/proc/%d/status' % pid).read_text().splitlines():
            if line.startswith(('VmHWM:', 'VmPeak:', 'VmRSS:')):
                key, value = line.split(':', 1)
                out[key.lower() + '_kb'] = int(value.split()[0])
        for line in Path('/proc/%d/io' % pid).read_text().splitlines():
            key, value = line.split(':')
            out['io_' + key] = int(value)
        fields = Path('/proc/%d/stat' % pid).read_text().split()
        out['cpu_ms'] = round((int(fields[13]) + int(fields[14])) * 10.0, 1)
    except Exception as exc:
        out['proc_metrics_error'] = str(exc)
    return out

def sample_rss(pid, peak):
    while True:
        try:
            out = subprocess.run(['ps', '-o', 'rss=', '-p', str(pid)], capture_output=True, text=True, timeout=1)
            value = int(out.stdout.strip() or 0) * 1024
            if value > peak['bytes']:
                peak['bytes'] = value
        except Exception:
            pass
        time.sleep(0.01)

def timed(s, sql):
    t0 = time.perf_counter_ns()
    resp = s.request(sql, mode='exact')
    ms = (time.perf_counter_ns() - t0) / 1e6
    assert resp.get('ok') is True, resp
    return resp, ms

def verify(resp, truth, j):
    assert resp['columns'] == ['id', 'd', 'body'], resp['columns']
    rows = resp['rows']
    assert len(rows) == len(truth[j]), (j, len(rows), len(truth[j]))
    for (ident, dist), row in zip(truth[j], rows):
        assert row[0] == ident, (j, row[0], ident)
        assert math.isclose(row[1], dist, rel_tol=1e-10, abs_tol=1e-10), (j, row[1], dist)

if __name__ == '__main__':
    main()
