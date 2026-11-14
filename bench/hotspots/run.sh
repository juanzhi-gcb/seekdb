#!/usr/bin/env bash
set -euo pipefail
repo=$(git rev-parse --show-toplevel)
result="$repo/hotspots-results"
mkdir "$result"
scratch=$(mktemp -d "$RUNNER_TEMP/mvdb-hotspots-XXXXXX")
mkdir "$scratch/data" "$scratch/tmp"
chmod 0777 "$scratch/data" "$scratch/tmp"
export TMPDIR="$scratch/tmp" PYTHONDONTWRITEBYTECODE=1
trap 'sudo chown -R "$(id -u):$(id -g)" "$result"' EXIT
lscpu > "$result/lscpu.txt"
for item in reference candidate; do
 ref=a24d2cd764027adec7371f268f99ccdca2307032
 [[ "$item" == reference ]] || ref=08caace24793b51ad1ce9a698e010b8388bd9a32
 printf '%s\n' "$ref" > "$result/$item-commit.txt"
 mkdir "$scratch/$item"
 git archive "$ref" Dockerfile CMakeLists.txt src include tests | tar -x -C "$scratch/$item"
 python3 bench/hotspots/instrument.py "$scratch/$item"
 docker build -t "mvdb:hotspots-$item" "$scratch/$item" > "$result/build-$item.log" 2>&1
 docker image inspect --format '{{.Id}}' "mvdb:hotspots-$item" > "$result/$item-image.txt"
done
g++ -std=c++20 -O2 -ffp-contract=off bench/jsonl_oracle.cpp -o "$scratch/oracle"
for profile in grid-bigint continuous-bigint cluster-bigint; do
 shape=${profile%-*};schema=${profile#*-}
 sudo env MVDB_SCREEN_SHAPE="$shape" PYTHONPATH="$repo/bench" python3 -B bench/newa24/distribution.py --reference-image "$(cat "$result/reference-image.txt")" --candidate-image "$(cat "$result/candidate-image.txt")" --oracle-bin "$scratch/oracle" --output "$result/$profile" --data-root "$scratch/data/contract" --temp-root "$scratch/tmp/contract" --rounds 2 --rows 10000 --queries 300 --warmup 10 --filter-type "$schema" > "$result/$profile.log" 2>&1
done
sudo find "$result" -name oracle-input.bin -delete
sudo python3 bench/hotspots/summarize.py "$result"
