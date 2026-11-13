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
for kind in parent jsononly prefixonly candidate; do
  headers="$repo"; values="$repo"
  case "$kind" in
    parent) headers="$scratch/parent"; values="$scratch/parent";;
    jsononly) values="$scratch/parent";;
    prefixonly) headers="$scratch/parent";;
  esac
  g++ -std=c++20 -O3 -I"$headers/include" -I"$headers/src" bench/jsonsimd6000/probe.cpp "$values/src/types/value.cpp" -o "$scratch/probe-$kind"
done
for round in 0 1 2; do
  order='parent jsononly prefixonly candidate'
  if [ "$round" = 1 ]; then order='candidate prefixonly jsononly parent'; fi
  for kind in $order; do "$scratch/probe-$kind" > "$result/$round-$kind.jsonl"; done
done
printf '%s\n' 'Isolated JSON parser: original, JSON scan only, initial ASCII prefix only, both. No database T/B/M or official score.' > "$result/scope.txt"
