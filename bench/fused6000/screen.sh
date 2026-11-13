#!/usr/bin/env bash
set -euo pipefail
repo=$(git rev-parse --show-toplevel)
result="$repo/fused-results"
mkdir "$result"
scratch=$(mktemp -d "$RUNNER_TEMP/mvdb-front-XXXXXX")
mkdir "$scratch/parent"
parent=aed099071e6e1221bfd320f6f99bf4ce677e7cc3
git archive "$parent" src include | tar -x -C "$scratch/parent"
lscpu > "$result/lscpu.txt"
g++ --version > "$result/compiler.txt"
git rev-parse HEAD > "$result/candidate-commit.txt"
printf '%s\n' "$parent" > "$result/parent-commit.txt"
for kind in parent candidate; do
  source="$repo"
  flags=(-DMVDB_STREAM_PARSE=1)
  if test "$kind" = parent; then source="$scratch/parent"; fi
  g++ -std=c++20 -O3 -ffp-contract=off "${flags[@]}" -I"$source/include" -I"$source/src" bench/front6000/probe.cpp "$source/src/sql/lexer.cpp" "$source/src/sql/parser.cpp" "$source/src/sql/binder.cpp" "$source/src/types/value.cpp" "$source/src/storage/row_codec.cpp" -o "$scratch/probe-$kind"
done
for name in short_decimal vector_literal; do
 g++ -std=c++20 -O1 -g -fsanitize=address,undefined -ffp-contract=off -Iinclude "tests/${name}_tests.cpp" src/sql/lexer.cpp src/sql/parser.cpp src/sql/binder.cpp src/types/value.cpp src/storage/row_codec.cpp -o "$scratch/$name"
 "$scratch/$name" > "$result/$name-differential.txt"
done
for pair in 0 1 2; do
  order=(parent candidate)
  if test "$pair" = 1; then order=(candidate parent); fi
  for kind in "${order[@]}"; do "$scratch/probe-$kind" > "$result/$kind-$pair.jsonl"; done
done
python3 - "$result" <<'PY'
import json,statistics,sys
from pathlib import Path
r=Path(sys.argv[1]);groups={}
for kind in ['parent','candidate']:
 for p in r.glob(kind+'-*.jsonl'):
  for line in p.read_text().splitlines():
   v=json.loads(line);key='/'.join([v['form'],v['kind'],v['stage']]);groups.setdefault(key,{}).setdefault(kind,[]).append(v)
out={}
for key,g in groups.items():
 assert {v['digest'] for v in g['parent']}=={v['digest'] for v in g['candidate']}
 a=statistics.median(v['ms'] for v in g['parent']);b=statistics.median(v['ms'] for v in g['candidate']);out[key]={'parent_ms':a,'candidate_ms':b,'ratio':b/a,'samples':g}
(r/'comparison.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps({k:v['ratio'] for k,v in out.items()}))
PY
