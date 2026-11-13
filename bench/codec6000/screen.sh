#!/usr/bin/env bash
set -euo pipefail
repo=$(git rev-parse --show-toplevel)
result="$repo/codec-results"
mkdir "$result"
scratch=$(mktemp -d "$RUNNER_TEMP/j11-screen-XXXXXX")
mkdir "$scratch/parent"
git archive aed099071e6e1221bfd320f6f99bf4ce677e7cc3 src include | tar -x -C "$scratch/parent"
lscpu > "$result/lscpu.txt"
for name in compact_writer lossless_words; do
 g++ -std=c++20 -O1 -g -fsanitize=address,undefined -Iinclude "tests/${name}_tests.cpp" -o "$scratch/$name-test"
 "$scratch/$name-test" > "$result/$name-test.txt"
done
cmake -S . -B "$scratch/build" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS='-O1 -g -fsanitize=address,undefined' > "$result/configure.log" 2>&1
cmake --build "$scratch/build" --target packed_codec_tests --parallel 2 > "$result/build-packed.log" 2>&1
MVDB_TEST_DIR="$scratch/test-data" "$scratch/build/packed_codec_tests" > "$result/packed_codec-test.txt"
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
    cmp "$scratch/$round-$corpus-parent.db" "$scratch/$round-$corpus-candidate.db"
    sha256sum "$scratch/$round-$corpus-parent.db" "$scratch/$round-$corpus-candidate.db" >> "$result/file-parity.txt"
  done
done
printf '%s\n' 'Storage-only screen; cold fadvise is advisory; syscr/rchar are logical read counters, read_bytes is process physical read I/O. These are not complete JSONL T/B/M or official E.' > "$result/scope.txt"
