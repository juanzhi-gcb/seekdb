#!/usr/bin/env bash
set -euo pipefail
mkdir -p "$RUNNER_TEMP/mvdb-batch-bin" "$RUNNER_TEMP/mvdb-batch-data" batch-results
bin="$RUNNER_TEMP/mvdb-batch-bin"
data="$RUNNER_TEMP/mvdb-batch-data"
image=mvdb:batch-baseline
docker run --rm --network none --cpus 2 --memory 2g --memory-swap 2g -v "$PWD:/candidate:ro" -v "$bin:/out" --entrypoint sh "$image" -c 'g++ -std=c++20 -O3 -I/src/include /candidate/bench/score_diagnostic.cpp /build/libminivectordb_core.a -o /out/diagnostic'
for width in 4 8 16; do
  source=score_batch${width}_probe.cpp
  if [[ "$width" = 4 ]]; then source=score_batch_probe.cpp; fi
  docker run --rm --network none --cpus 2 --memory 2g --memory-swap 2g -v "$PWD:/candidate:ro" -v "$bin:/out" --entrypoint sh "$image" -c "g++ -std=c++20 -O3 -I/src/include /candidate/bench/$source /candidate/bench/score_pruning_check.cpp -o /out/check$width && /out/check$width && g++ -std=c++20 -O3 -I/src/include /candidate/bench/$source /candidate/bench/score_diagnostic.cpp /build/libminivectordb_core.a -o /out/batch$width"
done
for round in 1 2 3; do
  order='diagnostic batch4 batch8 batch16'
  if [[ "$round" = 2 ]]; then order='batch16 batch8 batch4 diagnostic'; fi
  for binary in $order; do
    for filter in bigint varchar; do
      run="r$round-$filter-$binary"
      args=()
      if [[ "$filter" = varchar ]]; then args+=(-e MVDB_TEXT_FILTER=1); fi
      docker run --rm --network none --cpus 2 --memory 2g --memory-swap 2g "${args[@]}" -v "$bin:/probe:ro" -v "$data:/data" --entrypoint "/probe/$binary" "$image" "/data/$run/db" 10000 1000 1 1 > "batch-results/$run.json"
      echo "RESULT $run"
      cat "batch-results/$run.json"
    done
  done
done
python3 - <<'PY'
import json,statistics
from pathlib import Path
p=Path('batch-results')
for filter in ['bigint','varchar']:
    base=None
    for variant in ['diagnostic','batch4','batch8','batch16']:
        ds=[json.loads((p/f'r{r}-{filter}-{variant}.json').read_text()) for r in [1,2,3]]
        for r,d in zip([1,2,3],ds):
            assert d['result_digest']==json.loads((p/f'r{r}-{filter}-diagnostic.json').read_text())['result_digest']
        med=statistics.median(d['query_total_ms'] for d in ds)
        if base is None:base=med
        print('SUMMARY '+json.dumps({'filter':filter,'variant':variant,'query_ms':[d['query_total_ms'] for d in ds],'median_ms':med,'relative_percent':(med/base-1)*100,'exact_results_agree':True}))
PY
