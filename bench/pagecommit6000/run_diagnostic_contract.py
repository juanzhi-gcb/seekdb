import json,time,sys
from pathlib import Path
import run_linux_contract as linux
original_load=linux.benchmark.load_transport

def traced_transport():
 Base,config,lock=original_load()
 class Session(Base):
  def __init__(self,*args,**kwargs):
   super().__init__(*args,**kwargs);self.trace_rows=[]
  def request(self,sql=None,*args,**kwargs):
   start=time.monotonic_ns()
   try:return super().request(sql,*args,**kwargs)
   finally:self.trace_rows.append({'id':self.sequence,'start_ns':start,'end_ns':time.monotonic_ns(),'kind':sql.split(None,1)[0] if sql else kwargs.get('op'),'distance_query':bool(sql and 'L2_DISTANCE' in sql),'sql_bytes':len(sql.encode()) if sql else 0})
  def close(self):
   try:return super().close()
   finally:Path(str(self.log.name)+'.requests.json').write_text(json.dumps(self.trace_rows))
 return Session,config,lock
linux.benchmark.load_transport=traced_transport
if __name__=='__main__':sys.exit(linux.benchmark.main())
