"""Compare guarded native texture/matrix variants against dynamic PPC execution."""
import os,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
cc=Path('C:/msys64/mingw64/bin/gcc.exe');out=ROOT/'build/tests/mex-fastpaths';out.mkdir(parents=True,exist_ok=True)
exe=out/'test.exe'
args=[str(cc),'-O2','-fno-strict-aliasing',*['-I'+str(ROOT/p) for p in ['native/runtime','native/vendor/include','build/native-game/generated']],*[str(ROOT/p) for p in ['native/host/gxrt/tests/mex_fastpaths_test.c','native/host/gxrt/mex_runtime.c','native/host/gxrt/locked_cache.c','native/vendor/src/core/cpu.c','native/vendor/src/loader.c']],'-lm','-o',str(exe)]
os.environ['PATH']=str(cc.parent)+';'+os.environ['PATH'];subprocess.run(args,cwd=ROOT,check=True)
env=os.environ.copy();env['MELEE_MEX_BASE_DOL']=str(ROOT/'data/GALE01/sys/main.dol');env['MELEE_UNICORN_LIBRARY']=str(ROOT/'build/pc/bin/unicorn.dll')
subprocess.run([str(exe),env['MELEE_MEX_BASE_DOL']],cwd=ROOT,env=env,check=True)
