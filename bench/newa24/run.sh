#!/usr/bin/env bash
set -euo pipefail
repo=$(git rev-parse --show-toplevel)
result="$repo/newa24-results"
mkdir "$result"
scratch=$(mktemp -d "$RUNNER_TEMP/mvdb-newa24-XXXXXX")
mkdir "$scratch/data" "$scratch/tmp" "$scratch/probe"
chmod 0777 "$scratch/data" "$scratch/tmp" "$scratch/probe"
export TMPDIR="$scratch/tmp" PYTHONDONTWRITEBYTECODE=1
trap 'sudo chown -R "$(id -u):$(id -g)" "$result"' EXIT
lscpu > "$result/lscpu.txt"
git rev-parse HEAD > "$result/candidate-commit.txt"
baseline=a24d2cd764027adec7371f268f99ccdca2307032
printf '%s\n' "$baseline" > "$result/baseline-commit.txt"
limits=(--network none --cpus 2 --memory 2g --memory-swap 2g --pids-limit 128 --cap-drop ALL --security-opt no-new-privileges)
for item in a24 candidate; do
  mkdir "$scratch/$item"
  ref=HEAD;[[ "$item" != a24 ]] || ref="$baseline"
  git archive "$ref" Dockerfile CMakeLists.txt src include tests | tar -x -C "$scratch/$item"
  docker build --target test -t "mvdb:newa24-$item-test" "$scratch/$item" > "$result/build-$item.log" 2>&1
  docker build -t "mvdb:newa24-$item" "$scratch/$item" >> "$result/build-$item.log" 2>&1
  docker image inspect --format '{{.Id}}' "mvdb:newa24-$item" > "$result/$item-image.txt"
  docker run --rm "${limits[@]}" --mount "type=bind,src=$repo/bench/newa24/storage_bytes_probe.cpp,dst=/probe.cpp,readonly" --mount "type=bind,src=$scratch/probe,dst=/out" --entrypoint sh "mvdb:newa24-$item-test" -c "g++ -std=c++20 -O2 -ffp-contract=off -I/src/include -I/src/tests /probe.cpp /build/libminivectordb_core.a -o /out/probe-$item" > "$result/bytes-build-$item.log" 2>&1
  docker run --rm "${limits[@]}" --mount "type=bind,src=$scratch/data,dst=/data" --mount "type=bind,src=$scratch/probe,dst=/out,readonly" --entrypoint "/out/probe-$item" "mvdb:newa24-$item-test" "/data/bytes-$item.db" "/data/logical-$item.bin" > "$result/bytes-$item.log" 2>&1
done
sudo cmp "$scratch/data/logical-a24.bin" "$scratch/data/logical-candidate.bin"
docker run --rm "${limits[@]}" --mount "type=bind,src=$scratch/data,dst=/data" --mount "type=bind,src=$scratch/probe,dst=/out,readonly" --entrypoint /out/probe-candidate mvdb:newa24-candidate-test /data/bytes-a24.db /data/logical-compat.bin read > "$result/legacy-compat.log" 2>&1
sudo cmp "$scratch/data/logical-a24.bin" "$scratch/data/logical-compat.bin"
sudo sha256sum "$scratch/data"/logical-*.bin > "$result/identical-logical-bytes.txt"
sudo sha256sum "$scratch/data/bytes-a24.db" "$scratch/data/bytes-candidate.db" > "$result/physical-storage-bytes.txt"
docker run --rm "${limits[@]}" --mount "type=bind,src=$scratch/data,dst=/data" mvdb:newa24-candidate-test > "$result/ctest.log" 2>&1
python3 -B bench/vendor/seekdb-round2-f4d4e26/protocol_check/run.py --image mvdb:newa24-candidate --output "$result/public" > "$result/public.log" 2>&1
g++ -std=c++20 -O2 -ffp-contract=off bench/jsonl_oracle.cpp -o "$scratch/oracle"
for profile in grid-bigint grid-varchar continuous-bigint cluster-bigint outlier-bigint small-short-bigint large-grid-bigint large-continuous-bigint; do
  shape=${profile%-*};schema=${profile##*-};rows=10000;queries=1000;body=256
  case "$profile" in
    small-short-*) shape=grid;rows=1000;body=0;;
    large-grid-*) shape=grid;rows=50000;queries=300;;
    large-continuous-*) shape=continuous;rows=50000;queries=300;;
  esac
  sudo env MVDB_SCREEN_SHAPE="$shape" MVDB_SCREEN_BODY="$body" PYTHONPATH="$repo/bench" python3 -B bench/newa24/distribution.py --reference-image "$(cat "$result/a24-image.txt")" --candidate-image "$(cat "$result/candidate-image.txt")" --oracle-bin "$scratch/oracle" --output "$result/$profile" --data-root "$scratch/data/contract" --temp-root "$scratch/tmp/contract" --rounds 3 --rows "$rows" --queries "$queries" --warmup 10 --filter-type "$schema" > "$result/$profile.log" 2>&1
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
