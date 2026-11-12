#!/usr/bin/env bash
set -euo pipefail
test "$(uname -m)" = x86_64
repo=$(git rev-parse --show-toplevel)
cd "$repo"
result="$repo/transport-results"
test ! -e "$result"
mkdir "$result"
scratch=$(mktemp -d "$RUNNER_TEMP/mvdb-transport-XXXXXX")
mkdir "$scratch/baseline" "$scratch/data" "$scratch/tmp"
chmod 0777 "$scratch/data" "$scratch/tmp"
export TMPDIR="$scratch/tmp" PYTHONDONTWRITEBYTECODE=1
trap 'sudo chown -R "$(id -u):$(id -g)" "$result"' EXIT
baseline=ff117199f8077de7a6e3e1f91a611397bb6641a0
git archive "$baseline" Dockerfile CMakeLists.txt src include tests | tar -x -C "$scratch/baseline"
git rev-parse HEAD > "$result/candidate-commit.txt"
printf '%s\n' "$baseline" > "$result/baseline-commit.txt"
lscpu > "$result/lscpu.txt"
uname -a > "$result/kernel.txt"
docker build -t mvdb:transport-base "$scratch/baseline" > "$result/build-base.log" 2>&1
docker build --target test -t mvdb:transport-test . > "$result/build-test.log" 2>&1
docker run --rm --network none --cpus 2 --memory 2g --memory-swap 2g --cap-drop ALL \
  --security-opt no-new-privileges --mount "type=bind,src=$scratch/data,dst=/data" \
  mvdb:transport-test > "$result/ctest.log" 2>&1
docker build -t mvdb:transport-candidate . > "$result/build-runtime.log" 2>&1
reference=$(docker image inspect --format '{{.Id}}' mvdb:transport-base)
candidate=$(docker image inspect --format '{{.Id}}' mvdb:transport-candidate)
printf '%s\n' "$reference" "$candidate" > "$result/image-identities.txt"
python3 -B bench/vendor/seekdb-round2-f4d4e26/protocol_check/run.py --image "$candidate" --output "$result/public" > "$result/public.log" 2>&1
g++ -std=c++20 -O2 -ffp-contract=off bench/jsonl_oracle.cpp -o "$scratch/oracle"
for schema in bigint varchar; do
  sudo python3 -B bench/run_linux_contract.py --reference-image "$reference" --candidate-image "$candidate" \
    --oracle-bin "$scratch/oracle" --output "$result/$schema" --data-root "$scratch/data/contract" \
    --temp-root "$scratch/tmp/contract" --rounds 3 --rows 10000 --queries 1000 --warmup 10 --filter-type "$schema" \
    2>&1 | tee "$result/$schema.log"
done
sudo python3 - "$result" <<'PY'
import json,sys
from pathlib import Path
root=Path(sys.argv[1]);out={}
for schema in ['bigint','varchar']:
    d=json.loads((root/schema/'summary.json').read_text());ratios={}
    for phase in ['write/unindexed','query/unindexed','query/indexed']:
        a=d['groups']['reference/'+phase]['median'];b=d['groups']['candidate/'+phase]['median']
        ratios[phase]={k:b[k]/a[k] for k in ['T_ms','B_bytes','M_bytes']}
    out[schema]={'ratios':ratios,'groups':d['groups'],'metadata':d['metadata']}
(root/'comparison.json').write_text(json.dumps(out,indent=2)+'\n')
print('TRANSPORT_COMPARISON '+json.dumps({k:v['ratios'] for k,v in out.items()}))
# Retain all measurements. Omit only regenerable full oracle input blobs.
for path in root.rglob('oracle-input.bin'):path.unlink()
PY
