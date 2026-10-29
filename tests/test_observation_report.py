#!/usr/bin/env python3
"""Parse mixed/incomplete logs; do not double-count cumulative snapshots."""
from pathlib import Path
import importlib.util,tempfile,json,csv
root=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('observation_report',root/'scripts/report_observation.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
def event(**kw):
 d=dict(type='span',kind='resource',key=1,name='resource|name',phase=1,frame=5,thread=1,start_us=1000,duration_us=2000,bytes=100,status=1,count=0,total_us=0,max_us=0,object=1);d.update(kw);return '[INFO] OBS '+json.dumps(d)
text='\n'.join([
 'old non-OBS log',event(type='asset',count=2,total_us=7000,max_us=5000,bytes=200),event(),
 event(type='asset',count=3,total_us=9000,max_us=5000,bytes=300),
 event(kind='boot',name='scene=MainMenu loading=0',start_us=0),
 event(kind='boot',name='scene=s_playingstage loading=1',phase=2,start_us=5000),
 event(kind='gc',duration_us=400000),event(type='frames',kind='boot',name='frames=3 p95_ms=2001',count=3,total_us=100000,max_us=400000),
 '[INFO] OBS health dropped=2 aggregate_overflow=1 queue=0 writer_us=100 max_write_us=20','[INFO] OBS {partial'
])
d=m.analyze(text);assert d['invalid_json']==1 and len(d['assets'])==1
assert d['assets'][0]['calls']==3 and d['assets'][0]['total_ms']==9 and d['assets'][0]['bytes']==300
assert len(d['phases'])==2 and not d['phases'][-1]['complete']
with tempfile.TemporaryDirectory() as tmp:
 out=Path(tmp);m.report(d,out)
 assert len(list(csv.DictReader((out/'assets.csv').open())))==1
 report=(out/'report.md').read_text();assert 'Há perda de cobertura' in report and 'resource/name' in report
 empty=m.analyze('legacy log');m.report(empty,out);assert 'não contém' in (out/'report.md').read_text()
print('PASS: mixed/partial logs, cumulative snapshots deduplicated, frame/phase/CSV/JSON output, missing telemetry and loss warnings')
