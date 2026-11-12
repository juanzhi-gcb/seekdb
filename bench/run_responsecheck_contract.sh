#!/usr/bin/env bash
set -euo pipefail
test "$(uname -m)" = x86_64
repo=$(git rev-parse --show-toplevel);cd "$repo"
result="$repo/responsecheck-results";mkdir "$result"
scratch=$(mktemp -d "$RUNNER_TEMP/mvdb-responsecheck-XXXXXX")
mkdir "$scratch/reference" "$scratch/original" "$scratch/data" "$scratch/tmp"
chmod 0777 "$scratch/data" "$scratch/tmp"
export TMPDIR="$scratch/tmp" PYTHONDONTWRITEBYTECODE=1
trap 'sudo chown -R "$(id -u):$(id -g)" "$result"' EXIT
reference=f8bbfeca9175602a2951bd7d2c378647f7392f47
original=f62ce8a7417b01e62061e389092b9d6d5df4c6c5
candidate=b170c05f3138a08f5c662f15c932bafc661901ab
test "$(git rev-parse HEAD:src)" = "$(git rev-parse "$candidate:src")"
test "$(git rev-parse HEAD:include)" = "$(git rev-parse "$candidate:include")"
printf '%s\n' "$original" "$reference" "$candidate" > "$result/source-identities.txt"
git archive "$reference" Dockerfile CMakeLists.txt src include tests | tar -x -C "$scratch/reference"
git archive "$original" Dockerfile CMakeLists.txt src include tests | tar -x -C "$scratch/original"
lscpu > "$result/lscpu.txt"
docker build -t mvdb:responsecheck-reference "$scratch/reference" > "$result/build-reference.log" 2>&1
docker build -t mvdb:responsecheck-original "$scratch/original" > "$result/build-original.log" 2>&1
docker build -t mvdb:responsecheck-candidate . > "$result/build-candidate.log" 2>&1
reference_image=$(docker image inspect --format '{{.Id}}' mvdb:responsecheck-reference)
original_image=$(docker image inspect --format '{{.Id}}' mvdb:responsecheck-original)
candidate_image=$(docker image inspect --format '{{.Id}}' mvdb:responsecheck-candidate)
printf '%s\n' "$reference_image" "$original_image" "$candidate_image" > "$result/image-identities.txt"
g++ -std=c++20 -O2 -ffp-contract=off bench/jsonl_oracle.cpp -o "$scratch/oracle"
for schema in bigint varchar; do
  for shape in capacity latency combined; do
    rows=30000;queries=100;against="$reference_image"
    if [[ "$shape" = latency ]];then rows=100;queries=1000;fi
    if [[ "$shape" = combined ]];then rows=10000;queries=1000;against="$original_image";fi
    sudo python3 -B bench/run_linux_contract.py --reference-image "$against" --candidate-image "$candidate_image" \
      --oracle-bin "$scratch/oracle" --output "$result/$shape-$schema" --data-root "$scratch/data/contract" \
      --temp-root "$scratch/tmp/contract" --rounds 3 --rows "$rows" --queries "$queries" --warmup 10 --filter-type "$schema" \
      > "$result/$shape-$schema.log" 2>&1
  done
done
sudo python3 - "$result" <<'PY'
import json,sys
from pathlib import Path
root=Path(sys.argv[1]);out={'scope':'Unchanged b170 engine; capacity outlier follow-up,100-row latency diagnostic, direct f62 comparison; no official E conversion','comparisons':{}}
for p in sorted(root.glob('*/summary.json')):
 d=json.loads(p.read_text());ratios={}
 for phase in ['write/unindexed','query/unindexed','query/indexed']:
  a=d['groups']['reference/'+phase]['median'];b=d['groups']['candidate/'+phase]['median']
  ratios[phase]={k:b[k]/a[k] for k in ['T_ms','B_bytes','M_bytes']}
 out['comparisons'][p.parent.name]={'ratios':ratios,'groups':d['groups'],'metadata':d['metadata']}
(root/'comparison.json').write_text(json.dumps(out,indent=2)+'\n')
print('FOCUS_COMPARISON '+json.dumps({k:v['ratios'] for k,v in out['comparisons'].items()}))
for p in root.rglob('oracle-input.bin'):p.unlink()
PY
