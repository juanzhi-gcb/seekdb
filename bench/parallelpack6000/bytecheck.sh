#!/usr/bin/env bash
set -euo pipefail
repo=$(git rev-parse --show-toplevel)
result="$repo/pack-results"
mkdir "$result"
scratch=$(mktemp -d "$RUNNER_TEMP/mvdb-pack-XXXXXX")
mkdir "$scratch/parent" "$scratch/candidate"
chmod 0777 "$scratch/parent" "$scratch/candidate"
docker build --target test -t mvdb:pack-parent-test "$1" > "$result/build-parent-test.log" 2>&1
limits=(--network none --cpus 2 --memory 2g --memory-swap 2g --pids-limit 128 --cap-drop ALL --security-opt no-new-privileges)
for version in parent candidate; do
 image=mvdb:certified-test
 if test "$version" = parent;then image=mvdb:pack-parent-test;fi
 docker run --rm "${limits[@]}" --mount "type=bind,src=$repo/bench/parallelpack6000/store_probe.cpp,dst=/probe.cpp,readonly" --mount "type=bind,src=$scratch/$version,dst=/out" --entrypoint sh "$image" -c 'g++ -std=c++20 -O3 -ffp-contract=off -I/src/include /probe.cpp /build/libminivectordb_core.a -pthread -o /out/probe' > "$result/build-$version-probe.log" 2>&1
 for corpus in 0 1;do
  docker run --rm "${limits[@]}" --mount "type=bind,src=$scratch/$version,dst=/out" --entrypoint /out/probe "$image" "/out/data-$corpus.db" "$corpus" > "$result/$version-$corpus.json"
 done
done
for corpus in 0 1;do
 cmp "$scratch/parent/data-$corpus.db" "$scratch/candidate/data-$corpus.db"
 sha256sum "$scratch/parent/data-$corpus.db" "$scratch/candidate/data-$corpus.db" >> "$result/byte-identical-sha256.txt"
done
