#!/usr/bin/env bash
set -euo pipefail
repo=$(git rev-parse --show-toplevel)
result="$repo/drain-probe-results"
mkdir "$result"
scratch=$(mktemp -d "$RUNNER_TEMP/mvdb-drain-probe-XXXXXX")
mkdir "$scratch/source" "$scratch/data" "$scratch/tmp"
chmod 0777 "$scratch/data" "$scratch/tmp"
export TMPDIR="$scratch/tmp" PYTHONDONTWRITEBYTECODE=1
trap 'sudo chown -R "$(id -u):$(id -g)" "$result"' EXIT
uname -a > "$result/kernel.txt"
lscpu > "$result/lscpu.txt"
git archive a24d2cd764027adec7371f268f99ccdca2307032 Dockerfile CMakeLists.txt src include tests | tar -x -C "$scratch/source"
sed -i 's/cmake --build \/build --parallel 2/cmake --build \/build --target minivectordb --parallel 2/' "$scratch/source/Dockerfile"
docker build -t mvdb:drain-a24 "$scratch/source" > "$result/build.log" 2>&1
image=$(docker image inspect --format '{{.Id}}' mvdb:drain-a24)
g++ -std=c++20 -O2 -ffp-contract=off bench/jsonl_oracle.cpp -o "$scratch/oracle"
sudo env MVDB_SCREEN_SHAPE=grid MVDB_SCREEN_BODY=256 PYTHONPATH="$repo/bench" python3 -B bench/drain-probe/probe.py --reference-image "$image" --candidate-image "$image" --oracle-bin "$scratch/oracle" --output "$result/repro" --data-root "$scratch/data/contract" --temp-root "$scratch/tmp/contract" --rounds 12 --rows 10000 --queries 300 --warmup 10 --filter-type bigint > "$result/repro.log" 2>&1
