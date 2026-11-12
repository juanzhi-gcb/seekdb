#!/usr/bin/env bash
# Native diagnostics only. Do not derive an official E or Prompt score here.
set -euo pipefail
test "$(uname -m)" = x86_64
: "${RUNNER_TEMP:?GitHub runner scratch directory is required}"
: "${BASELINE_REF:?}" "${RANGE_REF:?}" "${WRITE_REF:?}" "${STORE_REF:?}" "${COMBINED_REF:?}"
replica=${NATIVE_REPLICA:-1}
[[ "$replica" = 1 || "$replica" = 2 ]]
[[ "${RUN_OVERFLOW:-1}" = 0 || "${RUN_OVERFLOW:-1}" = 1 ]]
repo=$(git rev-parse --show-toplevel)
cd "$repo"
result="$repo/native-candidate-results-$replica"
test ! -e "$result"
mkdir "$result"
scratch=$(mktemp -d "$RUNNER_TEMP/mvdb-native-candidates-$replica-XXXXXX")
mkdir -p "$scratch/sources" "$scratch/harness" "$scratch/bin" "$scratch/data" "$scratch/tmp"
export TMPDIR="$scratch/tmp" TMP="$scratch/tmp" TEMP="$scratch/tmp" PYTHONDONTWRITEBYTECODE=1
finish() {
  local status=$?
  trap - EXIT
  sudo chown -R "$(id -u):$(id -g)" "$result" || true
  exit "$status"
}
trap finish EXIT
variants=(baseline range write store combined)
declare -A refs test_images runtime_images
refs[baseline]=$BASELINE_REF;refs[range]=$RANGE_REF;refs[write]=$WRITE_REF
refs[store]=$STORE_REF;refs[combined]=$COMBINED_REF

# Record selected public host facts only; never dump the environment/secrets.
uname -a > "$result/uname.txt"
lscpu --json > "$result/lscpu.json"
lscpu > "$result/lscpu.txt"
cat /proc/meminfo > "$result/meminfo.txt"
findmnt --json --target "$scratch/data" > "$result/filesystem.json"
df -Th "$scratch/data" > "$result/disk.txt"
docker version > "$result/docker-version.txt"
docker info --format '{{json .}}' > "$result/docker-info.json"
git rev-parse HEAD > "$result/harness-checkout.txt"
printf '%s\n' "$scratch" > "$result/scratch-path.txt"
python3 -B - "$result" "$replica" <<'PY'
import json,os,sys
from pathlib import Path
refs={name.lower():os.environ[name+'_REF'] for name in ['BASELINE','RANGE','WRITE','STORE','COMBINED']}
Path(sys.argv[1],'refs.json').write_text(json.dumps({'replica':int(sys.argv[2]),'refs':refs},indent=2)+'\n')
PY

for variant in "${variants[@]}"; do
  ref=${refs[$variant]}
  [[ "$ref" =~ ^[0-9a-f]{40}$ ]]
  mkdir -p "$result/$variant" "$scratch/sources/$variant" "$scratch/bin/$variant" "$scratch/data/$variant"
  if ! git cat-file -e "$ref^{commit}" 2>/dev/null; then
    git fetch --no-tags origin "$ref" > "$result/$variant/fetch.log" 2>&1
  fi
  test "$(git rev-parse "$ref^{commit}")" = "$ref"
  git show -s --format=fuller "$ref" > "$result/$variant/commit.txt"
  git archive "$ref" Dockerfile CMakeLists.txt include src tests | tar -x -C "$scratch/sources/$variant"
  (cd "$scratch/sources/$variant" && find Dockerfile CMakeLists.txt include src tests -type f -print0 |
    sort -z | xargs -0 sha256sum) > "$result/$variant/source-sha256.txt"
  # Only these private scratch mounts are made writable for cap-drop containers.
  chmod 0777 "$scratch/bin/$variant" "$scratch/data/$variant"
  tag="mvdb:native-$replica-$variant-test"
  docker build --progress=plain --target test -t "$tag" "$scratch/sources/$variant" \
    2>&1 | tee "$result/$variant/build-test.log"
  test_images[$variant]=$(docker image inspect --format '{{.Id}}' "$tag")
  docker image inspect "${test_images[$variant]}" > "$result/$variant/test-image.json"
  docker run --rm --network none --read-only --entrypoint sha256sum "${test_images[$variant]}" \
    /build/minivectordb /build/libminivectordb_core.a > "$result/$variant/build-binaries.sha256"
  docker run --rm --network none --read-only --entrypoint ctest "${test_images[$variant]}" \
    --test-dir /build --show-only=json-v1 > "$result/$variant/ctest-inventory.json"
done

# All variants use exactly the same probe, mutation trace, transport and oracle.
git archive "$COMBINED_REF" bench/score_kernel_diagnostic.cpp bench/jsonl_oracle.cpp \
  bench/run_linux_contract.py bench/run_contract_benchmark.py \
  bench/vendor/seekdb-round2-f4d4e26 tests/mutation_prefix_tests.cpp |
  tar -x -C "$scratch/harness"
harness="$scratch/harness"
sha256sum "$harness/bench/score_kernel_diagnostic.cpp" "$harness/bench/jsonl_oracle.cpp" \
  "$harness/bench/run_linux_contract.py" "$harness/bench/run_contract_benchmark.py" \
  "$harness/tests/mutation_prefix_tests.cpp" bench/native_range_isa.cpp bench/native_options.cpp \
  bench/run_native_candidates.sh bench/summarize_native_candidates.py > "$result/common-harness.sha256"
PYTHONPATH="$harness/bench" python3 -B - <<'PY' > "$result/vendor-integrity.txt"
import run_contract_benchmark as b
_,_,sha=b.load_transport()
print('Verified official package',b.UPSTREAM,'lock SHA256',sha)
PY

limits=(--network none --cpus 2 --memory 2g --memory-swap 2g --pids-limit 128
  --cap-drop ALL --security-opt no-new-privileges)
for variant in "${variants[@]}"; do
  # Probe uses the image's own headers/core, with one frozen harness source.
  docker run --rm "${limits[@]}" \
    --mount "type=bind,src=$harness,dst=/harness,readonly" \
    --mount "type=bind,src=$repo/bench/native_options.cpp,dst=/native_options.cpp,readonly" \
    --mount "type=bind,src=$scratch/bin/$variant,dst=/out" \
    --mount "type=bind,src=$scratch/data/$variant,dst=/data" \
    --env TMPDIR=/data --entrypoint sh "${test_images[$variant]}" -c '
      set -eu
      g++ -std=c++20 -O3 -ffp-contract=off -I/src/include /harness/bench/score_kernel_diagnostic.cpp /build/libminivectordb_core.a -o /out/probe
      g++ -std=c++20 -O2 -I/src/include /harness/tests/mutation_prefix_tests.cpp /build/libminivectordb_core.a -o /out/mutation-prefix
      g++ -std=c++20 -O2 -I/src/include /native_options.cpp -o /out/options
      /out/options > /out/options.json
      sha256sum /out/probe /out/mutation-prefix /out/options
    ' > "$result/$variant/compile-probes.log" 2>&1
  cp "$scratch/bin/$variant/options.json" "$result/$variant/options.json"
done

# The full suite remains exactly the combined source's 51 CTest cases.
python3 -B - "$result/combined/ctest-inventory.json" <<'PY'
import json,sys
data=json.load(open(sys.argv[1]));assert len(data['tests'])==51,len(data['tests'])
PY
docker run --rm "${limits[@]}" \
  --mount "type=bind,src=$scratch/data/combined,dst=/data" --env TMPDIR=/data \
  --env PYTHONDONTWRITEBYTECODE=1 "${test_images[combined]}" \
  ctest --test-dir /build --output-on-failure --no-tests=error \
  2>&1 | tee "$result/combined/ctest.log"

# Force each supported native SIMD path, rather than accepting a silent fallback.
for variant in range combined; do
  docker run --rm "${limits[@]}" \
    --mount "type=bind,src=$repo/bench/native_range_isa.cpp,dst=/native_range_isa.cpp,readonly" \
    --mount "type=bind,src=$scratch/bin/$variant,dst=/out" \
    --mount "type=bind,src=$scratch/data/$variant,dst=/data" --env TMPDIR=/data \
    --entrypoint sh "${test_images[$variant]}" -c '
      g++ -std=c++20 -O2 -ffp-contract=off -I/src/include -I/src/tests -I/src /native_range_isa.cpp /build/libminivectordb_core.a -Wl,--wrap=pread -o /out/range-isa
    ' > "$result/$variant/compile-range-isa.log" 2>&1
  docker run --rm "${limits[@]}" \
    --mount "type=bind,src=$scratch/bin/$variant,dst=/probe,readonly" \
    --mount "type=bind,src=$scratch/data/$variant,dst=/data" \
    --env TMPDIR=/data --entrypoint /probe/range-isa "${test_images[$variant]}" /data/range-isa \
    > "$result/$variant/range-isa.json" 2> "$result/$variant/range-isa.stderr.log"
done

# Same expanded trace linked separately to every core; compare immutable bytes.
for variant in "${variants[@]}"; do
  docker run --rm "${limits[@]}" \
    --mount "type=bind,src=$scratch/bin/$variant,dst=/probe,readonly" \
    --mount "type=bind,src=$scratch/data/$variant,dst=/data" \
    --entrypoint /probe/mutation-prefix "${test_images[$variant]}" /data/mutation-db \
    > "$result/$variant/mutation-prefix.log" 2>&1
done
for format in format02 format03 format04-unpacked format04-packed; do
  for variant in "${variants[@]}"; do
    file="$scratch/data/$variant/mutation-db/$format.db"
    sudo sha256sum "$file" >> "$result/$variant/mutation-files.sha256"
    if [[ "$variant" != baseline ]]; then
      sudo cmp "$scratch/data/baseline/mutation-db/$format.db" "$file" \
        >> "$result/$variant/mutation-cmp.log" 2>&1
      printf 'PASS byte identity: baseline/%s == %s/%s\n' "$format" "$variant" "$format" \
        >> "$result/$variant/mutation-cmp.log"
    fi
  done
done

for variant in baseline combined; do
  tag="mvdb:native-$replica-$variant-runtime"
  docker build --progress=plain --target runtime -t "$tag" "$scratch/sources/$variant" \
    2>&1 | tee "$result/$variant/build-runtime.log"
  runtime_images[$variant]=$(docker image inspect --format '{{.Id}}' "$tag")
  docker image inspect "${runtime_images[$variant]}" > "$result/$variant/runtime-image.json"
  docker run --rm --network none --read-only --entrypoint sha256sum "${runtime_images[$variant]}" \
    /usr/local/bin/minivectordb > "$result/$variant/runtime-binary.sha256"
  test "$(head -1 "$result/$variant/build-binaries.sha256" | cut -d' ' -f1)" = \
       "$(cut -d' ' -f1 "$result/$variant/runtime-binary.sha256")"
done
python3 -B "$harness/bench/vendor/seekdb-round2-f4d4e26/protocol_check/run.py" \
  --image "${runtime_images[combined]}" --output "$result/combined/public" \
  2>&1 | tee "$result/combined/public.log"
python3 -B - "$result/combined/public/result.json" <<'PY'
import json,sys
report=json.load(open(sys.argv[1]))
assert report['passed'] and len(report['cases'])==20
assert all(case['status']=='PASS' for case in report['cases'])
PY

# Three complete interleaved rounds on EACH host, two schema types, five cores.
# TEXT_FILTER is a presence flag. For bigint it must be absent, never '=0'.
mkdir "$result/micro"
for round in 1 2 3; do
  case "$round" in
    1) order=(baseline range write store combined);;
    2) order=(combined store write range baseline);;
    3) order=(write store combined baseline range);;
  esac
  if [[ "$replica" = 2 ]]; then order=("${order[@]:2}" "${order[@]:0:2}"); fi
  for schema in bigint varchar; do
    for variant in "${order[@]}"; do
      label="r$round-$schema-$variant"
      args=(--env MVDB_BENCH_CGROUP=1)
      if [[ "$schema" = varchar ]]; then args+=(--env MVDB_TEXT_FILTER=1); fi
      printf '%s\n' "$label" >> "$result/micro/order.txt"
      docker run --rm "${limits[@]}" "${args[@]}" \
        --mount "type=bind,src=$scratch/bin/$variant,dst=/probe,readonly" \
        --mount "type=bind,src=$scratch/data/$variant,dst=/data" \
        --env TMPDIR=/data --entrypoint /probe/probe "${test_images[$variant]}" \
        "/data/$label/db" 10000 1000 1 1 \
        > "$result/micro/$label.json" 2> "$result/micro/$label.stderr.log"
    done
  done
done

# Real JSONL protocol, independent float32 oracle and unchanged resource collector.
# Every root/output/oracle path is absolute and inside this job's own scratch tree.
g++ -std=c++20 -O2 -ffp-contract=off "$harness/bench/jsonl_oracle.cpp" -o "$scratch/jsonl-oracle"
sha256sum "$scratch/jsonl-oracle" > "$result/jsonl-oracle.sha256"
mkdir "$result/contract"
for schema in bigint varchar; do
  sudo python3 -B "$harness/bench/run_linux_contract.py" \
    --reference-image "${runtime_images[baseline]}" --candidate-image "${runtime_images[combined]}" \
    --oracle-bin "$scratch/jsonl-oracle" --output "$result/contract/10000-$schema" \
    --data-root "$scratch/data/contract" --temp-root "$scratch/tmp/contract" \
    --rounds 3 --rows 10000 --queries 1000 --warmup 10 --filter-type "$schema" \
    2>&1 | tee "$result/contract/10000-$schema.log"
done
if [[ "${RUN_OVERFLOW:-1}" = 1 ]]; then
  for schema in bigint varchar; do
    sudo python3 -B "$harness/bench/run_linux_contract.py" \
      --reference-image "${runtime_images[baseline]}" --candidate-image "${runtime_images[combined]}" \
      --oracle-bin "$scratch/jsonl-oracle" --output "$result/contract/30000-$schema" \
      --data-root "$scratch/data/contract" --temp-root "$scratch/tmp/contract" \
      --rounds 1 --rows 30000 --queries 100 --warmup 10 --filter-type "$schema" \
      2>&1 | tee "$result/contract/30000-$schema.log"
  done
fi
sudo chown -R "$(id -u):$(id -g)" "$result"
python3 -B bench/summarize_native_candidates.py "$result" | tee "$result/summary.log"
