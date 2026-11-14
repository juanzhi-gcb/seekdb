from pathlib import Path
import json,sys
root=Path(sys.argv[1]);rows=[]
for p in root.glob('*/*/stderr.log'):
 case=json.loads((p.parent/'result.json').read_text())
 for line in p.read_text().splitlines():
  try:d=json.loads(line)
  except ValueError:continue
  if d.get('diagnostic')!='aggregate':continue
  if d['kind']=='insert' and '-write-' not in p.parent.name:continue
  if d['kind']=='query_warm' and '-query-' not in p.parent.name:continue
  if d['kind'] not in ['insert','query_warm']:continue
  item={'profile':p.parent.parent.name,'case':p.parent.name,'kind':d['kind'],'client_T_ms':case['T_ms'],**d}
  item['stage_percent_of_server']={k:round(v['ns']/d['wall_ns']*100,3) for k,v in d['stages'].items()}
  item['server_ms']=d['wall_ns']/1e6;item['cpu_ms']=d['cpu_ns']/1e6
  rows.append(item)
assert rows
(root/'stage-results.json').write_text(json.dumps({'scope':'Instrumented diagnostic totals; nested stages must not be summed; original per-row profiler remains disabled. Warm query stages exclude first ten queries. Not a score comparison.','rows':rows},indent=2)+'\n')
print('Diagnostic rows',len(rows))
