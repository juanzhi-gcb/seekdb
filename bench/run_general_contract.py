#!/usr/bin/env python3
"""Alternative distribution for robustness, with the unchanged resource collector."""
import hashlib
import json
from pathlib import Path
import random
import struct
import sys
import run_linux_contract as linux

original=linux.benchmark.make_workload
def continuous(rows,queries,seed,filter_type):
    w=original(rows,queries,seed,filter_type)
    rng=random.Random(seed^0x81931)
    def f32():return struct.unpack('<f',struct.pack('<f',rng.uniform(-16,16)))[0]
    w['vectors']=[[f32() for _ in range(128)] for _ in range(rows)]
    w['query_vectors']=[[f32() for _ in range(128)] for _ in range(queries)]
    alphabet='abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789'
    w['bodies']=[''.join(rng.choice(alphabet) for _ in range(256)) for _ in range(rows)]
    order=list(range(rows));rng.shuffle(order)
    literal=lambda value:json.dumps(value,separators=(',',':'))
    group=lambda value:str(value) if filter_type=='bigint' else "'category-"+str(value)+"'"
    w['inserts']=[]
    for start in range(0,rows,100):
        records=["(%d,%s,'%s','%s')"%(i,group(i%10),literal(w['vectors'][i]),w['bodies'][i]) for i in order[start:start+100]]
        w['inserts'].append('INSERT INTO t VALUES '+','.join(records)+';')
    w['queries']=["SELECT id,L2_DISTANCE(v,'%s') AS d,body FROM t%s ORDER BY d LIMIT 10;"%
        (literal(v),' WHERE g='+group((j//2)%10) if j%2 else '') for j,v in enumerate(w['query_vectors'])]
    return w

linux.benchmark.make_workload=continuous
if __name__=='__main__':
    result=linux.benchmark.main()
    output=Path(sys.argv[sys.argv.index('--output')+1])
    metadata={'distribution':'continuous finite float32, random 256-character ASCII payload',
              'generator_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'resource_and_oracle_logic':'unchanged run_contract_benchmark.py via run_linux_contract.py'}
    (output/'distribution.json').write_text(json.dumps(metadata,indent=2)+'\n')
    sys.exit(result)
