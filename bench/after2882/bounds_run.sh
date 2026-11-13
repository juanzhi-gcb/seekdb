#!/usr/bin/env bash
set -euo pipefail
repo=$(git rev-parse --show-toplevel)
result="$repo/bounds-results"
mkdir "$result"
scratch=$(mktemp -d "$RUNNER_TEMP/mvdb-bounds-XXXXXX")
lscpu > "$result/lscpu.txt"
git rev-parse HEAD > "$result/candidate-commit.txt"
for version in old centered robust; do
 case "$version" in
 old) ref=dfee55b84284f4c39d2df45951cdbcb1d1baf1a2;;
 centered) ref=e124978e0f904fcc75c82936387f4811b18b8c91;;
 robust) ref=HEAD;;
 esac
 mkdir "$scratch/$version"
 git archive "$ref" src include tests | tar -x -C "$scratch/$version"
 g++ -std=c++20 -O3 -ffp-contract=off -I"$scratch/$version/include" -I"$scratch/$version/src" bench/after2882/bounds_probe.cpp "$scratch/$version/src/vector/exact.cpp" "$scratch/$version/src/types/value.cpp" -o "$scratch/$version/probe"
 done
for version in old centered robust; do "$scratch/$version/probe" > "$result/$version.jsonl"; done
for test in certified_candidates int8_bound; do
 g++ -std=c++20 -O1 -g -fsanitize=address,undefined -ffp-contract=off -Iinclude "tests/${test}_tests.cpp" src/vector/exact.cpp src/types/value.cpp -o "$scratch/$test"
 "$scratch/$test" > "$result/$test-tests.json"
done
cat "$result/old.jsonl" "$result/centered.jsonl" "$result/robust.jsonl"
