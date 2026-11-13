import os,time,json,statistics,tempfile,random,ctypes
from pathlib import Path
rng=random.Random(20261113)
payload=rng.randbytes(65536);root=rng.randbytes(4096)
results=[]
with tempfile.TemporaryDirectory(prefix="mvdb-sync-") as tmp:
 for round in range(4):
  modes=['fsync','reserve_fsync'] if round%2==0 else ['reserve_fsync','fsync']
  for mode in modes:
   p=Path(tmp)/(str(round)+mode)
   fd=os.open(p,os.O_CREAT|os.O_RDWR|os.O_EXCL,0o600)
   os.pwrite(fd,b'\0'*8192,0);os.fsync(fd)
   samples=[];cpu0=time.process_time_ns();wall0=time.perf_counter_ns()
   if mode=='reserve_fsync':
    libc=ctypes.CDLL(None,use_errno=True);libc.fallocate.argtypes=[ctypes.c_int,ctypes.c_int,ctypes.c_longlong,ctypes.c_longlong]
    assert libc.fallocate(fd,1,0,8192+512*len(payload))==0,ctypes.get_errno()
    assert os.fstat(fd).st_size==8192
   for i in range(512):
    t=time.perf_counter_ns()
    assert os.pwrite(fd,payload,8192+i*len(payload))==len(payload)
    assert os.pwrite(fd,root,(i%2)*4096)==4096
    os.fsync(fd)
    samples.append((time.perf_counter_ns()-t)/1e6)
   total=(time.perf_counter_ns()-wall0)/1e6;cpu=(time.process_time_ns()-cpu0)/1e6
   os.close(fd)
   with p.open('rb') as f:
    f.seek(8192);assert f.read(len(payload))==payload
    f.seek(8192+511*len(payload));assert f.read()==payload
   s=sorted(samples)
   r=dict(round=round,mode=mode,total_ms=total,cpu_ms=cpu,median_ms=statistics.median(s),p99_ms=s[int(.99*len(s))],max_ms=max(s),samples_ms=samples)
   results.append(r);print(json.dumps({k:v for k,v in r.items() if k!='samples_ms'}),flush=True)
   p.unlink()
Path('sync-probe-results.json').write_text(json.dumps({'scope':'native standalone append+root durability microprobe; not engine score; all waits included','results':results},indent=2))
