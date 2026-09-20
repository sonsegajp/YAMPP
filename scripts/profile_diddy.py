"""Measure actual submitted gameplay frames, not just host VI ticks."""
import argparse, csv, json, os, statistics, subprocess, sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--name',required=True);p.add_argument('--renderer',default='build/pc/bin/melee_aurora_mod.dll');p.add_argument('--capture',action='store_true');a=p.parse_args()
out=ROOT/'build/comparisons'/a.name;out.mkdir(parents=True,exist_ok=True)
reference=json.loads((ROOT/'build/comparisons/diddy-looping-movement/report.json').read_text())
env=os.environ.copy();env.update(MELEE_MODS='1',MELEE_MOD_REGISTRY=str(ROOT/'build/modkit/diddy-pipeline-check/mods/registry.tsv'),MELEE_AURORA_DLL=str(ROOT/a.renderer),MELEE_PERF_CSV=str(out/'frames.csv'))
for key in ['MELEE_MEMORY_CARD','MELEE_SETTINGS','MELEE_MOD_TEST_FIGHTER','MELEE_MOD_TEST_STAGE']:env.pop(key,None)
command=[sys.executable,str(ROOT/'scripts/run_native_check.py'),'--name',a.name,'--frames','4350','--executable','melee-mod.exe','--normal-exit','--timeline',reference['timeline']]
if a.capture:command.append('--capture')
with (out/'driver.log').open('w') as log:result=subprocess.run(command,cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT)
if result.returncode:raise SystemExit(result.returncode)
rows=[{k:float(v) for k,v in r.items()} for r in csv.DictReader((out/'frames.csv').open())]
def stats(x):
 x=sorted(x);return dict(mean=statistics.mean(x),p50=x[len(x)//2],p95=x[int(len(x)*.95)],max=x[-1])
summary={}
for name,lo,hi in [('css',1400,1900),('fight',2600,4300)]:
 rr=[r for r in rows if lo<=r['host']<=hi and r['rendered']]
 if len(rr)<2:raise RuntimeError('No rendered frames in '+name)
 summary[name]=dict(rendered=len(rr),fps=(len(rr)-1)/(rr[-1]['seconds']-rr[0]['seconds']),interval_ms=stats([(b['seconds']-r['seconds'])*1000 for r,b in zip(rr,rr[1:])]),**{k:stats([r[k] for r in rr]) for k in ['begin_ms','translate_ms','end_ms']})
log=(out/'run.log').read_text(errors='replace')
summary['diddy_loaded']='Diddy Lua timelines loaded' in log
if not summary['diddy_loaded']:raise RuntimeError('Input timeline did not load Diddy')
(out/'performance.json').write_text(json.dumps(summary,indent=2));print(json.dumps(summary,indent=2))
