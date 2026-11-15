#!/usr/bin/env python3
"""Distribution robustness screen; unchanged T/B/M collector and exact oracle."""
import os, sys, random, struct, json
import run_general_contract as general
linux=general.linux
continuous=general.continuous
def generate(rows,queries,seed,filter_type):
    shape=os.environ['MVDB_SCREEN_SHAPE']
    if shape=='grid':return general.original(rows,queries,seed,filter_type)
    w=continuous(rows,queries,seed,filter_type)
    rng=random.Random(seed^0x61e5)
    def f32(x): return struct.unpack('<f',struct.pack('<f',x))[0]
    if shape=='positive':
        w['vectors']=[[f32(rng.random()) for _ in range(128)] for _ in range(rows)]
        w['query_vectors']=[[f32(rng.random()) for _ in range(128)] for _ in range(queries)]
    elif shape=='cluster':
        w['vectors']=[[f32(1+rng.random()*0.001) for _ in range(128)] for _ in range(rows)]
        w['query_vectors']=[[f32(1+rng.random()*0.001) for _ in range(128)] for _ in range(queries)]
    elif shape=='outlier':
        w['vectors'][-1]=[f32(1000000) for _ in range(128)]
    elif shape!='continuous': raise ValueError(shape)
    order=list(range(rows));rng.shuffle(order)
    literal=lambda v:json.dumps(v,separators=(',',':'))
    group=lambda g:str(g) if filter_type=='bigint' else "'category-"+str(g)+"'"
    w['inserts']=['INSERT INTO t VALUES '+','.join("(%d,%s,'%s','%s')"%(i,group(i%10),literal(w['vectors'][i]),w['bodies'][i]) for i in order[start:start+100])+';' for start in range(0,rows,100)]
    w['queries']=["SELECT id,L2_DISTANCE(v,'%s') AS d,body FROM t%s ORDER BY d LIMIT 10;"%(literal(v),' WHERE g='+group((j//2)%10) if j%2 else '') for j,v in enumerate(w['query_vectors'])]
    return w
linux.benchmark.make_workload=generate
if __name__=='__main__': sys.exit(linux.benchmark.main())
