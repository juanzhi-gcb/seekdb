#!/usr/bin/env bash
set -euo pipefail
repo=$(git rev-parse --show-toplevel)
result="$repo/latency-results"
mkdir "$result"
scratch=$(mktemp -d "$RUNNER_TEMP/mvdb-latency-XXXXXX")
mkdir "$scratch/reference" "$scratch/candidate" "$scratch/data" "$scratch/tmp"
chmod 0777 "$scratch/data" "$scratch/tmp"
export TMPDIR="$scratch/tmp" PYTHONDONTWRITEBYTECODE=1
trap 'sudo chown -R "$(id -u):$(id -g)" "$result"' EXIT
lscpu > "$result/lscpu.txt"
for item in reference candidate; do
  if [[ "$item" == reference ]];then ref=a24d2cd764027adec7371f268f99ccdca2307032;else ref=6e52564c9890eb4c12ad503b891a14352247d1dd;fi
  git archive "$ref" Dockerfile CMakeLists.txt src include tests | tar -x -C "$scratch/$item"
  python3 bench/latency6000/instrument.py "$scratch/$item" "$repo/bench/latency6000/diagnostic_trace.cpp"
  docker build -t "mvdb:latency-$item" "$scratch/$item" > "$result/build-$item.log" 2>&1
  docker image inspect --format '{{.Id}}' "mvdb:latency-$item" > "$result/$item-image.txt"
done
base=$(cat "$result/reference-image.txt")
candidate=$(cat "$result/candidate-image.txt")
g++ -std=c++20 -O2 -ffp-contract=off bench/jsonl_oracle.cpp -o "$scratch/oracle"
sudo env PYTHONPATH="$repo/bench" python3 -B bench/latency6000/run_diagnostic_contract.py --reference-image "$base" --candidate-image "$candidate" --oracle-bin "$scratch/oracle" --output "$result/traced" --data-root "$scratch/data/contract" --temp-root "$scratch/tmp/contract" --rounds 3 --rows 10000 --queries 200 --warmup 10 --filter-type bigint > "$result/traced.log" 2>&1
sudo python3 bench/latency6000/summarize.py "$result"
sudo find "$result" -name oracle-input.bin -delete
