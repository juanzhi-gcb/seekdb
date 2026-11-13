import json,sys,statistics
from pathlib import Path
root=Path(sys.argv[1]);result=[]
for request_path in root.rglob('stderr.log.requests.json'):
 requests=json.loads(request_path.read_text());events=[]
 for line in request_path.with_name('stderr.log').read_text().splitlines():
  try:x=json.loads(line)
  except ValueError:continue
  if x.get('trace')=='request':events.append(x)
 for h in requests:
  if h['kind']!='INSERT' and not h['distance_query']:continue
  matches=[x for x in events if h['start_ns']<=x['start_ns']<=h['end_ns']]
  if len(matches)!=1:raise RuntimeError(('trace correspondence failed',str(request_path),h,len(matches)))
  e=matches[0];result.append({'case':str(request_path.parent.relative_to(root)),'id':h['id'],'kind':h['kind'],'client_ms':(h['end_ns']-h['start_ns'])/1e6,'server_ms':e['wall_ns']/1e6,'server_cpu_ms':e['cpu_ns']/1e6,'fsync_ms':e['fsync_ns']/1e6,'fsync_cpu_ms':e['fsync_cpu_ns']/1e6,'pwrite_ms':e['pwrite_ns']/1e6,'pread_ms':e['pread_ns']/1e6,'outside_server_ms':(h['end_ns']-h['start_ns']-e['wall_ns'])/1e6})
summary={'scope':'Instrumented diagnostics only; not performance score; timers identical for both versions','cases':len(result),'slowest':sorted(result,key=lambda x:x['client_ms'],reverse=True)[:40],'groups':{}}
for version in ['reference','candidate']:
 for kind in ['INSERT','SELECT']:
  rows=[x for x in result if version in x['case'] and x['kind']==kind]
  summary['groups'][version+'/'+kind]={k:{'sum':sum(x[k] for x in rows),'median':statistics.median(x[k] for x in rows),'max':max(x[k] for x in rows)} for k in ['client_ms','server_ms','server_cpu_ms','fsync_ms','fsync_cpu_ms','pwrite_ms','pread_ms','outside_server_ms']}
(root/'correlated-requests.json').write_text(json.dumps(result,indent=2)+'\n');(root/'diagnosis.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(summary))
