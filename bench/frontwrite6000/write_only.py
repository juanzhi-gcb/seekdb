"""Write-only diagnostics; preserves the existing full-row/restart oracle and drain.
This is not a complete correctness gate or a replacement for uninstrumented T.
"""
import argparse,json,os,sys,tempfile,hashlib
from pathlib import Path
import run_linux_contract as linux
import run_diagnostic_contract as traced
b=linux.benchmark
p=argparse.ArgumentParser(description=__doc__)
for name in ['reference-image','candidate-image','output','data-root','temp-root','oracle-bin']:p.add_argument('--'+name,required=True)
p.add_argument('--filter-type',choices=['bigint','varchar'],required=True)
a=p.parse_args()
for name in ['output','data_root','temp_root','oracle_bin']:
 setattr(a,name,Path(getattr(a,name)));assert getattr(a,name).is_absolute()
a.rounds=5;a.rows=10000;a.queries=1;a.warmup=0;a.seed=20261110;a.local_case_budget=600
for path in [a.data_root,a.temp_root]:path.mkdir(parents=True,exist_ok=True)
a.output.mkdir(parents=True,exist_ok=False)
os.environ.update(TMPDIR=str(a.temp_root),TMP=str(a.temp_root),TEMP=str(a.temp_root),PYTHONDONTWRITEBYTECODE='1');tempfile.tempdir=str(a.temp_root)
Session,config,lock=b.load_transport()
for image in [a.reference_image,a.candidate_image]:b.require_image(image)
w=b.make_workload(a.rows,a.queries,a.seed,a.filter_type);truth,identity=b.oracle(w,a.oracle_bin,a.output)
metadata={'scope':__doc__,'args':{k:str(v) if isinstance(v,Path) else v for k,v in vars(a).items()},'oracle':identity,'official_config':config,'vendor_lock':lock,'diagnostic_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
b.write_json(a.output/'metadata.json',metadata)
samples=[]
for round in range(1,a.rounds+1):
 images=[('reference',a.reference_image),('candidate',a.candidate_image)]
 if round%2==0:images.reverse()
 for version,image in images:
  samples.append(b.run_one(a,Session,config,w,truth,version,image,round,'write',False))
  b.write_json(a.output/'summary.json',{'metadata':metadata,'groups':b.summarize(samples)})
