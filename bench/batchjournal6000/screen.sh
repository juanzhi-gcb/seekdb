#!/usr/bin/env bash
set -euo pipefail
repo=$(git rev-parse --show-toplevel)
result="$repo/batchjournal-results"
mkdir "$result"
scratch=$(mktemp -d "$RUNNER_TEMP/j11-screen-XXXXXX")
mkdir "$scratch/parent"
git archive 334ebac19109af2423f2621489cb778749b885a3 src include | tar -x -C "$scratch/parent"
lscpu > "$result/lscpu.txt"
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -Iinclude tests/batch_directory_tests.cpp -o "$scratch/codec-test"
"$scratch/codec-test" > "$result/codec-test.txt"
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -Iinclude tests/journal_batch_tests.cpp src/storage/store.cpp -Wl,--wrap=pwrite -Wl,--wrap=fsync -o "$scratch/journal-test"
"$scratch/journal-test" > "$result/journal-test.txt"
for kind in parent candidate; do
  source="$repo"
  if [ "$kind" = parent ]; then source="$scratch/parent"; fi
  g++ -std=c++20 -O3 -ffp-contract=off -I"$source/include" -I"$source/src" bench/batchjournal6000/store_probe.cpp "$source/src/storage/store.cpp" -o "$scratch/probe-$kind"
done
for round in 0 1 2; do
  order='parent candidate'
  if [ "$round" = 1 ]; then order='candidate parent'; fi
  for corpus in 0 1; do
    for kind in $order; do
      "$scratch/probe-$kind" "$scratch/$round-$corpus-$kind.db" "$corpus" > "$result/$round-$corpus-$kind.jsonl"
    done
  done
done
printf '%s\n' 'Storage-only screen; cold fadvise is advisory; syscr/rchar are logical read counters, read_bytes is process physical read I/O. These are not complete JSONL T/B/M or official E.' > "$result/scope.txt"
