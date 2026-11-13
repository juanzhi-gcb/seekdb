#!/usr/bin/env bash
set -euo pipefail
test "$(uname -m)" = x86_64
mkdir integer-rank-results
lscpu > integer-rank-results/lscpu.txt
g++ --version > integer-rank-results/compiler.txt
scratch=$(mktemp -d "$RUNNER_TEMP/integer-rank-XXXXXX")
mkdir "$scratch/reference" "$scratch/candidate"
git archive d1376f67875c5c504525e0972bcdd42e6657e660 src include tests bench/certified_kernel_probe.cpp | tar -x -C "$scratch/reference"
git archive HEAD src include tests bench/certified_kernel_probe.cpp | tar -x -C "$scratch/candidate"
for name in reference candidate;do
  dir="$scratch/$name"
  g++ -std=c++20 -O3 -ffp-contract=off -I"$dir/include" -I"$dir/src" "$dir/bench/certified_kernel_probe.cpp" "$dir/src/vector/exact.cpp" "$dir/src/types/value.cpp" -o "$scratch/$name-probe"
done
for test in int8_bound_tests certified_candidates_tests;do
  g++ -std=c++20 -O3 -ffp-contract=off -I"$scratch/candidate/include" "$scratch/candidate/tests/$test.cpp" "$scratch/candidate/src/vector/exact.cpp" "$scratch/candidate/src/types/value.cpp" -o "$scratch/$test"
  "$scratch/$test" > "integer-rank-results/$test.json"
done
for round in 1 2 3;do
  if ((round%2));then order='reference candidate';else order='candidate reference';fi
  for name in $order;do "$scratch/$name-probe" > "integer-rank-results/$name-$round.jsonl";done
done
python3 - <<'PY'
import json,statistics
from pathlib import Path
r=Path('integer-rank-results');out={'scope':'Native kernel screen only, not end-to-end T/B/M or official score','groups':{}}
for distribution in ['grid','continuous','equal']:
 values={}
 for version in ['reference','candidate']:
  rows=[json.loads(line) for p in r.glob(version+'-*.jsonl') for line in p.read_text().splitlines()]
  rows=[x for x in rows if x.get('distribution')==distribution and x['candidate']==1]
  assert len(rows)==9 and all(x['exact'] for x in rows)
  values[version]={'median_ms':statistics.median(x['ms'] for x in rows),'samples':rows}
 values['candidate_reference_ratio']=values['candidate']['median_ms']/values['reference']['median_ms'];out['groups'][distribution]=values
(r/'summary.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps(out))
PY
