"""Replay SDL input in an isolated Melee test window; verify XML persistence."""
import ctypes as C, ctypes.wintypes as W, os, sys, time, subprocess, json
from pathlib import Path
from project_config import configuration,disc_path,project_path,ROOT
user32=C.windll.user32
user32.PostMessageW.argtypes=[W.HWND,W.UINT,W.WPARAM,W.LPARAM]
user32.GetWindowThreadProcessId.argtypes=[W.HWND,C.POINTER(W.DWORD)]
root=ROOT/'build/comparisons/f1-controls';root.mkdir(parents=True,exist_ok=True)
settings=root/'settings.xml'
settings.write_text('<melee-settings schema="1" width="960" height="720" renderScale="1" widescreen="0" fullscreen="0" vsync="0" volume="100" mute="0" showFps="0" />')
env=os.environ.copy();env.update(MELEE_DISC=str(disc_path()),MELEE_FST=str(project_path(configuration()['assets']['directory'])/'sys/fst.bin'),MELEE_AURORA_DLL=str(ROOT/'build/pc/bin/melee_aurora_mod.dll'),MELEE_SETTINGS=str(settings),GCN_AURORA_HIDDEN='1',GCN_AURORA_DATA_DIR=str(root/'cache'),MELEE_INPUT='0:0:1',MELEE_RENDERDOC_FRAME='700',MELEE_RENDERDOC_PATH=str(ROOT/'build/renderdoc/f1-controls'))
env['PATH']=str(ROOT/'build/pc/bin')+';C:/msys64/mingw64/bin;'+env['PATH']
for key in ['MELEE_TEST_SETTINGS_OPEN','MELEE_MEMORY_CARD','MELEE_TEST_FAST_EXIT']:env.pop(key,None)
rd=Path(os.environ.get('RENDERDOC_PATH', str(ROOT/'build/tools/renderdoc/extracted/PFiles/RenderDoc')))
env.update(VK_LAYER_PATH=str(rd),VK_INSTANCE_LAYERS='VK_LAYER_RENDERDOC_Capture',ENABLE_VULKAN_RENDERDOC_CAPTURE='1')
events=[]
def key(frame,scan):events.extend([(frame,'key',scan,1,0),(frame+2,'key',scan,0,0)])
def click(frame,x,y):events.extend([(frame,'mouse',x,y,0),(frame+2,'button',1,1,0),(frame+4,'button',1,0,0)])
key(60,58);click(90,272,207);click(120,350,281);click(150,272,304);click(180,272,327);click(210,545,162);click(240,400,220);click(270,545,185);click(300,400,244);click(330,272,231);click(390,272,231);click(420,272,254)
input_file=root/'input.csv';input_file.write_text('\n'.join(','.join(map(str,e)) for e in sorted(events)))
env['MELEE_TEST_SDL_INPUT']=str(input_file)
with (root/'run.log').open('w') as log:
 process=subprocess.Popen([str(ROOT/'build/native-game/melee-mod.exe'),str(project_path(configuration()['assets']['directory'])/'sys/main.dol'),'1200'],cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT)
 process.wait(timeout=40)
 snapshot=settings.read_text();(root/'after-input.xml').write_text(snapshot)
import xml.etree.ElementTree as ET
state=ET.fromstring(snapshot)
assert state.get('widescreen')=='1' and state.get('mute')=='1' and state.get('showFps')=='1' and int(state.get('volume'))<100 and state.get('width')=='1280' and state.get('renderScale')=='2' and state.get('fullscreen')=='0' and state.get('vsync')=='1',state.attrib
report={'passed':True,'exit':process.returncode,'settings':snapshot,'input':'SDL event replay'}
(root/'report.json').write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2))
