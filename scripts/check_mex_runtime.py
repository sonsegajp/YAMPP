"""Build and run a real dynamic-PPC/native callback boundary regression."""
import os,subprocess,json,re
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
cc=Path('C:/msys64/mingw64/bin/gcc.exe');out=ROOT/'build/tests/mex-runtime';out.mkdir(parents=True,exist_ok=True)
exe=out/'mex-runtime-test.exe'
args=[str(cc),'-O1','-g','-fno-strict-aliasing','-I'+str(ROOT/'native/runtime'),'-I'+str(ROOT/'native/vendor/include'),'-I'+str(ROOT/'build/native-game/generated'),str(ROOT/'native/host/gxrt/tests/mex_runtime_test.c'),str(ROOT/'native/host/gxrt/mex_runtime.c'),str(ROOT/'native/host/gxrt/locked_cache.c'),str(ROOT/'native/vendor/src/core/cpu.c'),str(ROOT/'native/vendor/src/loader.c'),'-lm','-o',str(exe)]
os.environ['PATH']=str(cc.parent)+';'+os.environ['PATH']
subprocess.run(args,cwd=ROOT,check=True)
env=os.environ.copy();env['MELEE_MEX_BASE_DOL']=str(ROOT/'data/GALE01/sys/main.dol');env['MELEE_UNICORN_LIBRARY']=str(ROOT/'build/pc/bin/unicorn.dll');env['PATH']=str(cc.parent)+';'+env['PATH']
results=[]
for mode in ('0','1'):
    case=dict(env,MELEE_MEX_SCALAR_BATCH=mode)
    result=subprocess.run([str(exe),env['MELEE_MEX_BASE_DOL']],cwd=ROOT,env=case,check=True,capture_output=True,text=True)
    (out/('batch-'+mode+'.log')).write_text(result.stdout+result.stderr)
    state=re.search(r'm-ex state: ([0-9a-f]+)',result.stdout).group(1)
    seconds=float(re.search(r'benchmark: ([0-9.]+) seconds',result.stdout).group(1))
    results.append(dict(batch=mode,state=state,seconds=seconds))
assert results[0]['state']==results[1]['state'],results
(out/'report.json').write_text(json.dumps(dict(passed=True,runs=results),indent=2))
print(json.dumps(dict(passed=True,runs=results),indent=2))
