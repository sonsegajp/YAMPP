"""Native asset smoke through the real CSS, isolated from user packages/card."""
import os,sys,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[1];mods=root/'build/modkit/diddy-pipeline-check/mods'
os.environ['MELEE_WORKSHOP_TEST_MODS']=str(mods);sys.path.insert(0,str(root/'tools/modkit'));import catalog;catalog.runtime_registry()
env=os.environ.copy();env['MELEE_MODS']='1';env['MELEE_MOD_REGISTRY']=str(mods/'registry.tsv');env['MELEE_AURORA_DLL']=str(root/'build/pc/bin/melee_aurora_mod.dll')
for key in ['MELEE_MEMORY_CARD','MELEE_SETTINGS','MELEE_MOD_TEST_FIGHTER','MELEE_MOD_TEST_STAGE']:env.pop(key,None)
timeline='300:100:8,420:100:8,600:1000:8,840:1000:8,1020:4:8,1110:100:8,1200:100:8,1600:0:22:0:80,1700:100:8,1770:0:11:80:0,1800:0:7:0:-80,1860:100:8,2000:1000:8,2190:0:20:0:80,2230:0:20:80:0,2310:100:8'
r=subprocess.run([sys.executable,str(root/'scripts/run_native_check.py'),'--name',sys.argv[1],'--frames',os.environ.get('DIDDY_CHECK_FRAMES','3300'),'--executable','melee-mod.exe','--normal-exit','--capture','--timeline',timeline+os.environ.get('DIDDY_CHECK_INPUT','')],env=env,cwd=root)
sys.exit(r.returncode)
