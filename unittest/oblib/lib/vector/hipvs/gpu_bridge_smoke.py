#!/usr/bin/env python3
"""Real AMD bridge ABI smoke; never substitutes a CPU fixture or fallback."""
import argparse
import ctypes as c
import hashlib
import json
import math
from pathlib import Path
import struct
import time


class Config(c.Structure):
    _fields_ = [("size", c.c_uint32), ("dimension", c.c_uint32), ("rows", c.c_uint64),
                ("max_degree", c.c_uint32), ("ef_construction", c.c_uint32),
                ("metric", c.c_uint32), ("cpu_threads", c.c_uint32),
                ("device", c.c_int32), ("reserved", c.c_uint32)]


class Stats(c.Structure):
    _fields_ = [(name, c.c_uint32) for name in
                ("size", "backend", "gpu_build_submissions", "synchronized")] + [
                    ("gpu_build_ms", c.c_double), ("bridge_wall_ms", c.c_double),
                    ("device_name", c.c_char * 128), ("version", c.c_char * 128)]


WRITE = c.CFUNCTYPE(c.c_int, c.c_void_p, c.c_void_p, c.c_uint64)
BUILD = c.CFUNCTYPE(c.c_int, c.POINTER(Config), c.POINTER(c.c_float), WRITE,
                   c.c_void_p, c.POINTER(Stats), c.c_char_p, c.c_uint64)


class Api(c.Structure):
    _fields_ = [("version", c.c_uint32), ("size", c.c_uint32), ("build", BUILD)]


def check_hnsw(data, vectors, n, d, m):
    header = struct.Struct("<6QiI3QdQ")
    (offset0, capacity, count, stride, label_offset, vector_offset, maxlevel,
     entrypoint, maxm, maxm0, stored_m, mult, ef) = header.unpack_from(data)
    assert count == n and capacity >= n, (capacity, count)
    assert (maxm, maxm0, stored_m) == (m, 2 * m, m), (maxm, maxm0, stored_m)
    assert vector_offset == 4 + 8 * m and label_offset == vector_offset + 4 * d
    assert stride == label_offset + 8 and offset0 == 0
    assert 1 <= maxlevel < 64 and entrypoint < n and math.isfinite(mult) and ef >= m
    labels = []
    edges = 0
    for row in range(n):
        base = header.size + row * stride
        degree = struct.unpack_from("<I", data, base)[0]
        assert 0 < degree <= 2 * m, degree
        neighbors = struct.unpack_from("<" + "I" * degree, data, base + 4)
        assert all(v < n for v in neighbors) and len(set(neighbors)) == degree
        assert row not in neighbors
        edges += degree
        label = struct.unpack_from("<Q", data, base + label_offset)[0]
        labels.append(label)
        # Parallel CPU hierarchy insertion may reorder internal slots. Public
        # labels, rather than slots, identify the original input vector.
        assert label < n
        assert data[base + vector_offset:base + label_offset] == bytes(vectors)[label*d*4:(label+1)*d*4]
    assert sorted(labels) == list(range(n)), "labels must be original dense ordinals"
    offset = header.size + n * stride
    upper_nodes = 0
    observed_max = 0
    for row in range(n):
        length = struct.unpack_from("<I", data, offset)[0]
        offset += 4
        assert length % (4 + 4 * m) == 0
        levels = length // (4 + 4 * m)
        observed_max = max(observed_max, levels)
        upper_nodes += levels > 0
        for level in range(levels):
            at = offset + level * (4 + 4 * m)
            degree = struct.unpack_from("<I", data, at)[0]
            assert degree <= m
            neighbors = struct.unpack_from("<" + "I" * degree, data, at + 4)
            assert all(v < n for v in neighbors) and row not in neighbors
        offset += length
    assert observed_max == maxlevel and upper_nodes > 0 and offset == len(data)
    return {"rows": count, "dimension": d, "m": stored_m, "maxlevel": maxlevel,
            "upper_nodes": upper_nodes, "base_edges": edges, "export_bytes": len(data),
            "export_sha256": hashlib.sha256(data).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--rows", type=int, default=512)
    parser.add_argument("--dimension", type=int, default=32)
    parser.add_argument("--repetitions", type=int, default=3)
    args = parser.parse_args()
    out = Path(args.output_dir)
    out.mkdir(parents=True, exist_ok=True)
    library = c.CDLL(str(Path(args.bridge).resolve()))
    library.seekdb_hipvs_get_api_v1.restype = c.POINTER(Api)
    api = library.seekdb_hipvs_get_api_v1().contents
    assert api.version == 1 and api.size == c.sizeof(Api)
    n, d, m = args.rows, args.dimension, 16
    vectors = (c.c_float * (n * d))(*(
        math.sin(i * 0.17 + j * 0.31) + math.cos(i * 0.013 * (j + 1))
        for i in range(n) for j in range(d)))
    (out / "vectors.f32").write_bytes(bytes(vectors))
    records = []

    def invoke(metric, rows=n, reject=False, device=0, reused_stats=None, null_request=False):
        config = Config(c.sizeof(Config), d, rows, m, 200, metric, 4, device, 0)
        stats = reused_stats if reused_stats is not None else Stats()
        if reused_stats is None:
            stats.size = c.sizeof(Stats)
        error = c.create_string_buffer(8192)
        chunks = []

        @WRITE
        def receive(context, ptr, length):
            if reject:
                return 1
            chunks.append(c.string_at(ptr, length))
            return 0

        started = time.monotonic()
        status = api.build(None if null_request else c.byref(config), vectors, receive,
                           None, c.byref(stats), error, len(error))
        return status, stats, error.value.decode(errors="replace"), b"".join(chunks), time.monotonic()-started

    for metric in (1, 2):
        for repetition in range(args.repetitions):
            status, stats, error, data, wall = invoke(metric)
            assert status == 0, f"GPU build failed metric={metric}: {error}"
            assert (stats.backend, stats.gpu_build_submissions, stats.synchronized) == (1, 1, 1)
            assert stats.gpu_build_ms > 0 and stats.bridge_wall_ms > 0 and stats.device_name
            version = stats.version.decode()
            assert "c_api_sha256=" in version and "source=" in version, version
            (out / f"metric-{metric}-run-{repetition}.hnsw").write_bytes(data)
            record = check_hnsw(data, vectors, n, d, m)
            record.update(metric=metric, repetition=repetition, device=stats.device_name.decode(),
                          version=version, gpu_event_span_ms=stats.gpu_build_ms,
                          bridge_wall_ms=stats.bridge_wall_ms, caller_wall_seconds=wall,
                          backend=stats.backend, gpu_build_submissions=stats.gpu_build_submissions,
                          synchronized=stats.synchronized, ef_construction=200, cpu_threads=4)
            records.append(record)
            print(json.dumps(record), flush=True)
    successful_stats = bytes(stats)
    for name, metric, kwargs, nonfinite in [
            ("reused-stats-small-input", 1, {"rows": 128}, False),
            ("reused-stats-invalid-metric", 99, {}, False),
            ("reused-stats-null-request", 1, {"null_request": True}, False),
            ("reused-stats-non-finite", 1, {}, True)]:
        reused = Stats.from_buffer_copy(successful_stats)
        original = vectors[0]
        if nonfinite:
            vectors[0] = math.nan
        try:
            status, returned, error, data, wall = invoke(metric, reused_stats=reused, **kwargs)
        finally:
            vectors[0] = original
        assert status != 0 and not data, (name, status, error)
        actual = (reused.backend, reused.gpu_build_submissions, reused.synchronized,
                  reused.gpu_build_ms, reused.bridge_wall_ms)
        assert actual == (0, 0, 0, 0, 0), (name, "stale success stats", actual)
        assert reused.size == c.sizeof(Stats) and not reused.device_name and not reused.version
        records.append({"negative_case": name, "status": status, "error": error,
                        "backend": reused.backend, "gpu_build_submissions": reused.gpu_build_submissions,
                        "synchronized": reused.synchronized, "gpu_build_ms": reused.gpu_build_ms,
                        "bridge_wall_ms": reused.bridge_wall_ms})
        print(json.dumps(records[-1]), flush=True)
    wrong_size = Stats.from_buffer_copy(successful_stats)
    wrong_size.size -= 1
    before = bytes(wrong_size)
    status, returned, error, data, wall = invoke(1, reused_stats=wrong_size)
    assert status != 0 and not data and bytes(wrong_size) == before
    records.append({"negative_case": "invalid-stats-size-untouched", "status": status, "error": error})
    print(json.dumps(records[-1]), flush=True)
    error = c.create_string_buffer(8192)
    status = api.build(None, None, WRITE(), None, None, error, len(error))
    assert status != 0 and error.value
    records.append({"negative_case": "null-stats", "status": status, "error": error.value.decode()})
    print(json.dumps(records[-1]), flush=True)
    for name, kwargs, expected in [
            ("small-input", {"rows": 128}, "no CPU fallback"),
            ("rejected-output", {"reject": True}, "callback rejected"),
            ("invalid-device", {"device": -1}, "")]:
        status, stats, error, data, wall = invoke(1, **kwargs)
        assert status != 0 and expected in error and not data, (name, status, error)
        assert stats.backend != 1, (name, stats.backend)
        records.append({"negative_case": name, "status": status, "error": error})
        print(json.dumps(records[-1]), flush=True)
    original = vectors[0]
    vectors[0] = math.nan
    try:
        status, stats, error, data, wall = invoke(1)
        assert status != 0 and "non-finite" in error and not data and stats.backend != 1
        records.append({"negative_case": "non-finite-input", "status": status, "error": error})
        print(json.dumps(records[-1]), flush=True)
    finally:
        vectors[0] = original
    # A failed callback/device/input must not poison subsequent builds in the process.
    status, stats, error, data, wall = invoke(1)
    assert status == 0 and stats.backend == 1 and stats.synchronized == 1, error
    recovery = check_hnsw(data, vectors, n, d, m)
    recovery.update(case="success-after-failures", backend=stats.backend,
                    gpu_build_submissions=stats.gpu_build_submissions, synchronized=stats.synchronized)
    records.append(recovery)
    print(json.dumps(recovery), flush=True)
    result = {"scope": "independent real GPU bridge smoke; not SQL acceptance or speedup",
              "bridge_sha256": hashlib.sha256(Path(args.bridge).read_bytes()).hexdigest(),
              "input_sha256": hashlib.sha256(bytes(vectors)).hexdigest(),
              "records": records}
    (out / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print("PASS: real GPU full-HNSW bridge smoke", flush=True)


if __name__ == "__main__":
    main()
