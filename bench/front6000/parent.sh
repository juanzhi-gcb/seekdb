#!/usr/bin/env bash
set -euo pipefail
repo=$(git rev-parse --show-toplevel)
result="$repo/parent-results"
mkdir "$result"
scratch=$(mktemp -d "$RUNNER_TEMP/mvdb-parent-XXXXXX")
mkdir "$scratch/parent" "$scratch/data" "$scratch/tmp"
chmod 0777 "$scratch/data" "$scratch/tmp"
export TMPDIR="$scratch/tmp" PYTHONDONTWRITEBYTECODE=1
trap 'sudo chown -R "$(id -u):$(id -g)" "$result"' EXIT
parent=aed099071e6e1221bfd320f6f99bf4ce677e7cc3
git archive "$parent" Dockerfile CMakeLists.txt src include tests | tar -x -C "$scratch/parent"
docker build -t mvdb:direct-parent "$scratch/parent" > "$result/build-parent.log" 2>&1
base=$(docker image inspect --format '{{.Id}}' mvdb:direct-parent)
candidate=$(docker image inspect --format '{{.Id}}' mvdb:certified-candidate)
printf '%s\n' "$parent" > "$result/parent-commit.txt"
printf '%s\n' "$base" "$candidate" > "$result/image-identities.txt"
g++ -std=c++20 -O2 -ffp-contract=off bench/jsonl_oracle.cpp -o "$scratch/oracle"
for schema in bigint varchar; do
  sudo python3 -B bench/run_linux_contract.py --reference-image "$base" --candidate-image "$candidate" \
    --oracle-bin "$scratch/oracle" --output "$result/default-$schema" --data-root "$scratch/data/contract" \
    --temp-root "$scratch/tmp/contract" --rounds 3 --rows 10000 --queries 1000 --warmup 10 --filter-type "$schema" > "$result/default-$schema.log" 2>&1
done
sudo python3 - "$result" <<'PY'
import json,sys
from pathlib import Path
root=Path(sys.argv[1]);out={'scope':'Uninstrumented native candidate vs J11 direct parent; not official score','comparisons':{}}
for p in sorted(root.glob('*/summary.json')):
 d=json.loads(p.read_text());ratios={}
 for phase in ['write/unindexed','query/unindexed','query/indexed']:
  a=d['groups']['reference/'+phase]['median'];b=d['groups']['candidate/'+phase]['median'];ratios[phase]={k:b[k]/a[k] for k in ['T_ms','B_bytes','M_bytes']}
 out['comparisons'][p.parent.name]={'ratios':ratios,'groups':d['groups'],'metadata':d['metadata']}
(root/'comparison.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps({k:v['ratios'] for k,v in out['comparisons'].items()}))
for p in root.rglob('oracle-input.bin'):p.unlink()
PY
