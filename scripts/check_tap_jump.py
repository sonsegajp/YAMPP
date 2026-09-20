"""Check native tap-jump hooks against the original recompiled predicates."""
from pathlib import Path
import os,subprocess,json,sys
from check_sdk_matrix_transfer import function
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/tests/tap-jump';OUT.mkdir(parents=True,exist_ok=True)
names=['800CAE80','800CAED0','800CAF78','800CB024','800CB804','800CB950','800D730C','800DF910']
body='\n'.join(function((ROOT/('build/native-game/generated/recomp_'+('0007' if a in ['800D730C','800DF910'] else '0006')+'.c')).read_text(encoding='utf-8'),'func_'+a) for a in names)
(OUT/'original.c').write_text('#include "recomp_funcs.h"\n'+body,encoding='utf-8')
compiler=os.environ.get('MELEE_GCC','C:/msys64/mingw64/bin/gcc.exe' if os.name=='nt' else 'gcc');env=os.environ.copy();env['PATH']=str(Path(compiler).parent)+os.pathsep+env.get('PATH','')
exe=OUT/('tap-jump-test.exe' if os.name=='nt' else 'tap-jump-test')
subprocess.run([compiler,'-O2','-I'+str(ROOT/'native/runtime'),'-I'+str(ROOT/'native/vendor/include'),'-I'+str(ROOT/'build/native-game/generated'),str(ROOT/'native/host/gxrt/tests/tap_jump_test.c'),str(OUT/'original.c'),'-o',str(exe)],cwd=ROOT,env=env,check=True)
r=subprocess.run([str(exe)],cwd=ROOT,env=env,capture_output=True,text=True)
report={'passed':r.returncode==0,'output':r.stdout+r.stderr};(OUT/'report.json').write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2));sys.exit(r.returncode)
