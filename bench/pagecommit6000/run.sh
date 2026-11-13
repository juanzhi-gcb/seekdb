#!/usr/bin/env bash
set -euo pipefail
repo=$(git rev-parse --show-toplevel)
result="$repo/pagecommit-results"
mkdir "$result"
scratch=$(mktemp -d "$RUNNER_TEMP/mvdb-latency-XXXXXX")
mkdir "$scratch/reference" "$scratch/candidate" "$scratch/data" "$scratch/tmp"
chmod 0777 "$scratch/data" "$scratch/tmp"
export TMPDIR="$scratch/tmp" PYTHONDONTWRITEBYTECODE=1
trap 'sudo chown -R "$(id -u):$(id -g)" "$result"' EXIT
lscpu > "$result/lscpu.txt"
for item in reference candidate; do
  if [[ "$item" == reference ]];then ref=55c6bffb4f84553e03d7fa8d30ea3fdef9986dce;else ref=HEAD;fi
  git archive "$ref" Dockerfile CMakeLists.txt src include tests | tar -x -C "$scratch/$item"
  python3 bench/pagecommit6000/instrument.py "$scratch/$item" "$repo/bench/pagecommit6000/diagnostic_trace.cpp"
  docker build -t "mvdb:latency-$item" "$scratch/$item" > "$result/build-$item.log" 2>&1
  docker image inspect --format '{{.Id}}' "mvdb:latency-$item" > "$result/$item-image.txt"
done
base=$(cat "$result/reference-image.txt")
candidate=$(cat "$result/candidate-image.txt")
g++ -std=c++20 -O2 -ffp-contract=off bench/jsonl_oracle.cpp -o "$scratch/oracle"
sudo env PYTHONPATH="$repo/bench" python3 -B bench/pagecommit6000/run_diagnostic_contract.py --reference-image "$base" --candidate-image "$candidate" --oracle-bin "$scratch/oracle" --output "$result/traced" --data-root "$scratch/data/contract" --temp-root "$scratch/tmp/contract" --rounds 5 --rows 10000 --queries 200 --warmup 10 --filter-type bigint > "$result/traced.log" 2>&1
sudo python3 bench/pagecommit6000/summarize.py "$result"
sudo find "$result" -name oracle-input.bin -delete
