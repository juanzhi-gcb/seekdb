#!/usr/bin/env bash
set -euo pipefail
repo=$(git rev-parse --show-toplevel)
result="$repo/regression-results"
mkdir "$result"
scratch=$(mktemp -d "$RUNNER_TEMP/mvdb-regression-XXXXXX")
mkdir "$scratch/data" "$scratch/tmp"
chmod 0777 "$scratch/data" "$scratch/tmp"
export TMPDIR="$scratch/tmp" PYTHONDONTWRITEBYTECODE=1
trap 'sudo chown -R "$(id -u):$(id -g)" "$result"' EXIT
lscpu > "$result/lscpu.txt"
git rev-parse HEAD > "$result/candidate-commit.txt"
for item in a24 dfee candidate; do
  mkdir "$scratch/$item"
  case "$item" in
    a24) ref=a24d2cd764027adec7371f268f99ccdca2307032;;
    dfee) ref=dfee55b84284f4c39d2df45951cdbcb1d1baf1a2;;
    candidate) ref=HEAD;;
  esac
  git archive "$ref" Dockerfile CMakeLists.txt src include tests | tar -x -C "$scratch/$item"
  docker build --target test -t "mvdb:regression-$item-test" "$scratch/$item" > "$result/build-$item.log" 2>&1
  docker build -t "mvdb:regression-$item" "$scratch/$item" >> "$result/build-$item.log" 2>&1
  docker image inspect --format '{{.Id}}' "mvdb:regression-$item" > "$result/$item-image.txt"
done
docker run --rm --network none --cpus 2 --memory 2g --memory-swap 2g --pids-limit 128 --cap-drop ALL --security-opt no-new-privileges --mount "type=bind,src=$scratch/data,dst=/data" mvdb:regression-candidate-test > "$result/ctest.log" 2>&1
python3 -B bench/vendor/seekdb-round2-f4d4e26/protocol_check/run.py --image mvdb:regression-candidate --output "$result/public" > "$result/public.log" 2>&1
g++ -std=c++20 -O2 -ffp-contract=off bench/jsonl_oracle.cpp -o "$scratch/oracle"
for shape in continuous positive cluster outlier; do
  for pair in a24-dfee dfee-candidate; do
    base=${pair%-*};candidate=${pair#*-}
    sudo env MVDB_SCREEN_SHAPE="$shape" PYTHONPATH="$repo/bench" python3 -B bench/after2882/distribution.py --reference-image "$(cat "$result/$base-image.txt")" --candidate-image "$(cat "$result/$candidate-image.txt")" --oracle-bin "$scratch/oracle" --output "$result/$shape-$pair" --data-root "$scratch/data/contract" --temp-root "$scratch/tmp/contract" --rounds 2 --rows 10000 --queries 200 --warmup 10 --filter-type bigint > "$result/$shape-$pair.log" 2>&1
  done
done
sudo find "$result" -name oracle-input.bin -delete
python3 - "$result" <<'PY'
import json,sys
from pathlib import Path
r=Path(sys.argv[1]);out={}
for p in sorted(r.glob('*/summary.json')):
 d=json.loads(p.read_text());v={}
 for phase in ['write/unindexed','query/unindexed','query/indexed']:
  a=d['groups']['reference/'+phase]['median'];b=d['groups']['candidate/'+phase]['median']
  v[phase]={'reference':a,'candidate':b,'ratio':{k:b[k]/a[k] for k in ['T_ms','B_bytes','M_bytes']}}
 out[p.parent.name]=v
(r/'comparison.json').write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps(out))
PY
