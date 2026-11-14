#!/usr/bin/env bash
set -euo pipefail
repo=$(git rev-parse --show-toplevel)
result="$repo/storage-trace-results"
mkdir "$result"
scratch=$(mktemp -d "$RUNNER_TEMP/mvdb-storage-trace-XXXXXX")
mkdir "$scratch/data" "$scratch/tmp"
chmod 0777 "$scratch/data" "$scratch/tmp"
export TMPDIR="$scratch/tmp" PYTHONDONTWRITEBYTECODE=1
trap 'sudo chown -R "$(id -u):$(id -g)" "$result"' EXIT
lscpu > "$result/lscpu.txt"
for item in reference candidate; do
 ref=a24d2cd764027adec7371f268f99ccdca2307032
 [[ "$item" == reference ]] || ref="$CANDIDATE_REF"
 printf '%s\n' "$ref" > "$result/$item-commit.txt"
 mkdir "$scratch/$item"
 git archive "$ref" Dockerfile CMakeLists.txt src include tests | tar -x -C "$scratch/$item"
 python3 bench/storage-latency/instrument.py "$scratch/$item"
 # Diagnostic image only: compile the runtime target, not all test executables.
 # Production/native scoring images are built by the separate full workflows.
 sed -i 's/cmake --build \/build --parallel 2/cmake --build \/build --target minivectordb --parallel 2/' "$scratch/$item/Dockerfile"
 docker build -t "mvdb:storage-trace-$item" "$scratch/$item" > "$result/build-$item.log" 2>&1
 docker image inspect --format '{{.Id}}' "mvdb:storage-trace-$item" > "$result/$item-image.txt"
done
g++ -std=c++20 -O2 -ffp-contract=off bench/jsonl_oracle.cpp -o "$scratch/oracle"
for shape in grid continuous outlier; do
 sudo env MVDB_SCREEN_SHAPE="$shape" MVDB_SCREEN_BODY=256 PYTHONPATH="$repo/bench" python3 -B bench/newa24/distribution.py --reference-image "$(cat "$result/reference-image.txt")" --candidate-image "$(cat "$result/candidate-image.txt")" --oracle-bin "$scratch/oracle" --output "$result/$shape" --data-root "$scratch/data/contract" --temp-root "$scratch/tmp/contract" --rounds 1 --rows 50000 --queries 300 --warmup 10 --filter-type bigint > "$result/$shape.log" 2>&1
done
sudo find "$result" -name oracle-input.bin -delete
