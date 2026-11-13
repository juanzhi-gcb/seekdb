#!/usr/bin/env bash
set -euo pipefail
repo=$(git rev-parse --show-toplevel)
mkdir jsonwide-results
result="$repo/jsonwide-results"
scratch=$(mktemp -d "$RUNNER_TEMP/jsonwide-XXXXXX")
mkdir "$scratch/parent"
git archive 334ebac19109af2423f2621489cb778749b885a3 src include | tar -x -C "$scratch/parent"
lscpu > "$result/lscpu.txt"
for suite in json_string text_validation; do
  g++ -std=c++20 -O1 -g -fsanitize=address,undefined -pthread -Iinclude "tests/${suite}_tests.cpp" src/types/value.cpp -o "$scratch/test-$suite"
  "$scratch/test-$suite" > "$result/$suite.txt"
done
for kind in parent candidate; do
  source="$repo"
  if [ "$kind" = parent ]; then source="$scratch/parent"; fi
  g++ -std=c++20 -O3 -I"$source/include" -I"$source/src" bench/jsonsimd6000/probe.cpp "$source/src/types/value.cpp" -o "$scratch/probe-$kind"
done
for round in 0 1 2; do
  order='parent candidate'
  if [ "$round" = 1 ]; then order='candidate parent'; fi
  for kind in $order; do "$scratch/probe-$kind" > "$result/$round-$kind.jsonl"; done
done
printf '%s\n' 'Isolated owned-string JSON parser only; no database T/B/M, no official E, no borrowed value semantics. Baseline334ebac1; candidate adds bounded AVX2 scan and32byte ASCII blocks, full fallback retained.' > "$result/scope.txt"
