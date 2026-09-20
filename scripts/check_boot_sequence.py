"""Capture native FMV/title startup with isolated initialized cards and real input."""
import argparse,json,os,re,shutil,subprocess,sys
from pathlib import Path
from project_config import ROOT,configuration,disc_path,game_environment,project_path

def main():
 ap=argparse.ArgumentParser(description=__doc__)
 ap.add_argument('--name',required=True);ap.add_argument('--content-root')
 ap.add_argument('--skip',action='store_true');ap.add_argument('--frames',type=int)
 ap.add_argument('--executable',default='build/native-game/YAMPP-vanilla-boot.exe')
 ap.add_argument('--renderer',default='build/pc/bin/melee_aurora_yampp_final.dll')
 a=ap.parse_args();out=(ROOT/'build/comparisons'/a.name).resolve()
 if not out.is_relative_to(ROOT/'build/comparisons'):ap.error('Invalid test folder')
 out.mkdir(parents=True,exist_ok=True);cfg=configuration();env=game_environment(cfg,disc_path(cfg))
 for k in list(env):
  if k.startswith(('MELEE_','GCN_')):env.pop(k)
 env.update(game_environment(cfg,disc_path(cfg)))
 for k in list(env):
  if k.startswith(('MELEE_TEST_','MELEE_CAPTURE_','MELEE_NETPLAY_','MELEE_CONTENT_','MELEE_MEX_','MELEE_RUNTIME_','MELEE_AUDIO_')) or k in ('MELEE_INPUT','MELEE_START_TITLE'):env.pop(k)
 settings=out/'settings.xml';settings.write_text('<melee-settings schema="1" width="640" height="480" renderScale="1" widescreen="0" fullscreen="0" vsync="0" volume="100" mute="1"/>')
 card=out/'card.raw';shutil.copy2(ROOT/('build/akaneia-content-user-test/card.raw' if a.content_root else 'build/netplay-work/template-card.raw'),card)
 record=None
 if a.content_root:
  content=(ROOT/a.content_root).resolve()
  if not content.is_relative_to(ROOT/'build/akaneia-content'):ap.error('Invalid content folder')
  v=json.loads((content/'verification.json').read_text());record={'directory':content.relative_to(ROOT).as_posix(),'isoSHA256':v['isoSHA256'],'enabled':True}
 state=out/'content-state.json';state.write_text(json.dumps({'schema':1,'akaneia':record}))
 env.update(MELEE_VANILLA_BOOT='1',MELEE_AURORA_DLL=str(ROOT/a.renderer),MELEE_SETTINGS=str(settings),MELEE_MEMORY_CARD=str(card),GCN_AURORA_HIDDEN='1',GCN_AURORA_DATA_DIR=str(out/'cache'),MELEE_CAPTURE_AURORA='1',MELEE_CAPTURE_UI='1',MELEE_CAPTURE_INTERVAL='300',MELEE_CAPTURE_DIR=str(out),MELEE_INPUT='900:1000:6' if a.skip else '0:0:1',MELEE_AUDIO_CAPTURE=str(out/'audio.pcm'),MELEE_CONTENT_STATE=str(state),MELEE_CONTENT_LAUNCHER='1',MELEE_CONTENT_RESTART=str(out/'restart.json'),MELEE_CONTENT_BOOT_PLAN=str(out/'boot-plan.env'),MELEE_CONTENT_PLAN_SCRIPT=str(ROOT/'scripts/prepare_content_runtime.py'),MELEE_WORKSHOP_PYTHON=sys.executable,MELEE_TEST_FAST_EXIT='1')
 with (out/'run.log').open('w') as log:
  subprocess.run([sys.executable,env['MELEE_CONTENT_PLAN_SCRIPT'],'--output',env['MELEE_CONTENT_BOOT_PLAN']],cwd=ROOT,env=env,stdout=log,stderr=log,check=True)
  result=subprocess.run([str(ROOT/a.executable),str(project_path(cfg['assets']['directory'])/'sys/main.dol'),str(a.frames or (1500 if a.skip else 6000))],cwd=ROOT,env=env,stdout=log,stderr=log,timeout=240,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
 text=(out/'run.log').read_text(errors='replace');report={'exit':result.returncode,'content':a.content_root,'skipWithStart':a.skip,'fault':'[fault]' in text or '[content-host-fault]' in text,'assertions':re.findall(r'^.*assertion .*failed.*$',text,re.M),'openingEntered':'[boot] native opening movie entered' in text,'captures':len(list(out.glob('frame_*.ppm')))}
 (out/'report.json').write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2))
 assert not report['exit'] and not report['fault'] and not report['assertions'] and report['openingEntered'] and report['captures'],report
if __name__=='__main__':main()
