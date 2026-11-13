#!/usr/bin/env bash
set -euo pipefail
repo=$(git rev-parse --show-toplevel)
result="$repo/frontwrite-results"
mkdir "$result"
scratch=$(mktemp -d "$RUNNER_TEMP/mvdb-frontwrite-XXXXXX")
mkdir "$scratch/reference" "$scratch/candidate" "$scratch/data" "$scratch/tmp"
chmod 0777 "$scratch/data" "$scratch/tmp"
export TMPDIR="$scratch/tmp" PYTHONDONTWRITEBYTECODE=1
trap 'sudo chown -R "$(id -u):$(id -g)" "$result"' EXIT
lscpu > "$result/lscpu.txt"
for item in reference candidate; do
  if [[ "$item" == reference ]];then ref=334ebac19109af2423f2621489cb778749b885a3;else ref=aed099071e6e1221bfd320f6f99bf4ce677e7cc3;fi
  printf '%s\n' "$ref" > "$result/$item-commit.txt"
  git archive "$ref" Dockerfile CMakeLists.txt src include tests | tar -x -C "$scratch/$item"
  python3 bench/pagecommit6000/instrument.py "$scratch/$item" "$repo/bench/pagecommit6000/diagnostic_trace.cpp"
  docker build -t "mvdb:frontwrite-$item" "$scratch/$item" > "$result/build-$item.log" 2>&1
  docker image inspect --format '{{.Id}}' "mvdb:frontwrite-$item" > "$result/$item-image.txt"
done
base=$(cat "$result/reference-image.txt")
candidate=$(cat "$result/candidate-image.txt")
g++ -std=c++20 -O2 -ffp-contract=off bench/jsonl_oracle.cpp -o "$scratch/oracle"
status=0
for schema in bigint varchar; do
 if ! sudo env PYTHONPATH="$repo/bench:$repo/bench/pagecommit6000" python3 -B bench/frontwrite6000/write_only.py --reference-image "$base" --candidate-image "$candidate" --oracle-bin "$scratch/oracle" --output "$result/$schema" --data-root "$scratch/data/contract" --temp-root "$scratch/tmp/contract" --filter-type "$schema" > "$result/$schema.log" 2>&1; then status=1; fi
done
sudo find "$result" -name oracle-input.bin -delete
exit "$status"
