"""Diagnose a failed resource drain; never convert a failed run into a score."""
import os,sys,json,time,stat,ctypes
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'newa24'))
import distribution
linux=distribution.linux
original=linux.benchmark.drain

def diagnose(resources,root,timeout=30,interval=.1):
 try:return original(resources,root,timeout=timeout,interval=interval)
 except TimeoutError:
  print('DRAIN_PROBE_KERNEL '+Path('/proc/version').read_text().strip(),flush=True)
  print('DRAIN_PROBE_START '+json.dumps(resources.sample()),flush=True)
  entries=[]
  for p in root.rglob('*'):
   if p.is_symlink():continue
   st=p.stat();entries.append({'path':str(p.relative_to(root)),'bytes':st.st_size,'blocks':st.st_blocks,'mode':st.st_mode})
  print('DRAIN_PROBE_FILES '+json.dumps(entries),flush=True)
  pids=(resources.path/'cgroup.procs').read_text().split();fds=[]
  for pid in pids:
   for fd in Path('/proc',pid,'fd').iterdir():
    try:
     target=os.readlink(fd);st=fd.stat();entry={'pid':pid,'fd':fd.name,'target':target,'size':st.st_size,'regular':stat.S_ISREG(st.st_mode)}
     if entry['regular']:
      handle=os.open(fd,os.O_RDONLY|os.O_CLOEXEC)
      try:os.fsync(handle);entry['fd_fsync']='success'
      finally:os.close(handle)
     fds.append(entry)
    except OSError as e:fds.append({'pid':pid,'fd':fd.name,'error':str(e)})
  print('DRAIN_PROBE_FDS '+json.dumps(fds),flush=True)
  print('DRAIN_PROBE_AFTER_FDS '+json.dumps(resources.sample()),flush=True)
  # Selected filesystem only; this happens AFTER the run was declared invalid.
  # Retain the exception even if this diagnostic makes counters settle.
  handle=os.open(root,os.O_RDONLY|os.O_DIRECTORY)
  try:
   libc=ctypes.CDLL(None,use_errno=True);libc.syncfs.argtypes=[ctypes.c_int];libc.syncfs.restype=ctypes.c_int
   start=time.monotonic();value=libc.syncfs(handle)
   print('DRAIN_PROBE_SYNCFS '+json.dumps({'result':value,'errno':ctypes.get_errno(),'ms':(time.monotonic()-start)*1000}),flush=True)
  finally:os.close(handle)
  for i in range(10):
   time.sleep(1);print('DRAIN_PROBE_AFTER_SYNCFS '+json.dumps(resources.sample()),flush=True)
  raise
linux.benchmark.drain=diagnose
if __name__=='__main__':sys.exit(linux.benchmark.main())
