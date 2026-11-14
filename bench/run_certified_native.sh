#!/usr/bin/env bash
set -euo pipefail
test "$(uname -m)" = x86_64
repo=$(git rev-parse --show-toplevel)
cd "$repo"
result="$repo/certified-results"
test ! -e "$result"
mkdir "$result"
scratch=$(mktemp -d "$RUNNER_TEMP/mvdb-certified-XXXXXX")
mkdir "$scratch/baseline" "$scratch/data" "$scratch/tmp" "$scratch/probe"
chmod 0777 "$scratch/data" "$scratch/tmp" "$scratch/probe"
export TMPDIR="$scratch/tmp" PYTHONDONTWRITEBYTECODE=1
trap 'sudo chown -R "$(id -u):$(id -g)" "$result"' EXIT
baseline=a24d2cd764027adec7371f268f99ccdca2307032
git archive "$baseline" Dockerfile CMakeLists.txt src include tests | tar -x -C "$scratch/baseline"
git rev-parse HEAD > "$result/candidate-commit.txt"
printf '%s\n' "$baseline" > "$result/baseline-commit.txt"
lscpu > "$result/lscpu.txt"
uname -a > "$result/kernel.txt"
limits=(--network none --cpus 2 --memory 2g --memory-swap 2g --pids-limit 128 --cap-drop ALL --security-opt no-new-privileges)
docker build --target test -t mvdb:certified-base-test "$scratch/baseline" > "$result/build-base-test.log" 2>&1
docker build --target test -t mvdb:certified-test . > "$result/build-test.log" 2>&1
docker run --rm "${limits[@]}" --mount "type=bind,src=$scratch/data,dst=/data" mvdb:certified-test > "$result/ctest.log" 2>&1
docker run --rm "${limits[@]}" --mount "type=bind,src=$repo/bench/certified_kernel_probe.cpp,dst=/probe.cpp,readonly" --mount "type=bind,src=$scratch/probe,dst=/out" --entrypoint sh mvdb:certified-test -c 'g++ -std=c++20 -O3 -ffp-contract=off -I/src/include -I/src/src /probe.cpp /build/libminivectordb_core.a -o /out/kernel' > "$result/kernel-build.log" 2>&1
docker run --rm "${limits[@]}" --mount "type=bind,src=$scratch/probe,dst=/out,readonly" --entrypoint /out/kernel mvdb:certified-test > "$result/kernel.jsonl" 2> "$result/kernel.stderr.log"

docker build -t mvdb:certified-base "$scratch/baseline" > "$result/build-base.log" 2>&1
docker build -t mvdb:certified-candidate . > "$result/build-candidate.log" 2>&1
base=$(docker image inspect --format '{{.Id}}' mvdb:certified-base)
candidate=$(docker image inspect --format '{{.Id}}' mvdb:certified-candidate)
printf '%s\n' "$base" "$candidate" > "$result/image-identities.txt"
python3 -B bench/vendor/seekdb-round2-f4d4e26/protocol_check/run.py --image "$candidate" --output "$result/public" > "$result/public.log" 2>&1
g++ -std=c++20 -O2 -ffp-contract=off bench/jsonl_oracle.cpp -o "$scratch/oracle"
for schema in bigint varchar; do
  sudo python3 -B bench/run_linux_contract.py --reference-image "$base" --candidate-image "$candidate" \
    --oracle-bin "$scratch/oracle" --output "$result/default-$schema" --data-root "$scratch/data/contract" \
    --temp-root "$scratch/tmp/contract" --rounds 3 --rows 10000 --queries 1000 --warmup 10 --filter-type "$schema" > "$result/default-$schema.log" 2>&1
  sudo python3 -B bench/run_general_contract.py --reference-image "$base" --candidate-image "$candidate" \
    --oracle-bin "$scratch/oracle" --output "$result/general-$schema" --data-root "$scratch/data/contract" \
    --temp-root "$scratch/tmp/contract" --rounds 1 --rows 10000 --queries 200 --warmup 10 --filter-type "$schema" > "$result/general-$schema.log" 2>&1
  sudo python3 -B bench/run_linux_contract.py --reference-image "$base" --candidate-image "$candidate" \
    --oracle-bin "$scratch/oracle" --output "$result/capacity-$schema" --data-root "$scratch/data/contract" \
    --temp-root "$scratch/tmp/contract" --rounds 1 --rows 30000 --queries 100 --warmup 10 --filter-type "$schema" > "$result/capacity-$schema.log" 2>&1
done
sudo python3 -B bench/run_linux_contract.py --reference-image "$base" --candidate-image "$candidate" \
  --oracle-bin "$scratch/oracle" --output "$result/ann-bigint" --data-root "$scratch/data/contract" \
  --temp-root "$scratch/tmp/contract" --rounds 1 --rows 10000 --queries 1000 --warmup 10 \
  --filter-type bigint --search-mode ann > "$result/ann-bigint.log" 2>&1
sudo python3 - "$result" <<'PY'
import json,sys
from pathlib import Path
root=Path(sys.argv[1]);out={'scope':'Local generated workloads on native x86; no official E conversion','comparisons':{}}
for p in sorted(root.glob('*/summary.json')):
    d=json.loads(p.read_text());ratios={}
    for phase in ['write/unindexed','query/unindexed','query/indexed']:
        a=d['groups']['reference/'+phase]['median'];b=d['groups']['candidate/'+phase]['median']
        ratios[phase]={k:b[k]/a[k] for k in ['T_ms','B_bytes','M_bytes']}
    out['comparisons'][p.parent.name]={'ratios':ratios,'groups':d['groups'],'metadata':d['metadata']}
(root/'comparison.json').write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps({k:v['ratios'] for k,v in out['comparisons'].items()}))
for path in root.rglob('oracle-input.bin'):path.unlink()
PY
