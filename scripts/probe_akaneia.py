"""Boot the user's locally patched official Akaneia release in an isolated runtime."""
from pathlib import Path
import argparse,hashlib,json,os,re,shutil,struct,subprocess,time
from project_config import ROOT,configuration,disc_path,game_environment

def main():
 ap=argparse.ArgumentParser();ap.add_argument('--name',default='akaneia-first-boot');ap.add_argument('--frames',type=int,default=2400);ap.add_argument('--input');ap.add_argument('--seed-from');ap.add_argument('--stage',type=int);ap.add_argument('--content-root');ap.add_argument('--menu');ap.add_argument('--audio',action='store_true');ap.add_argument('--no-capture',action='store_true');ap.add_argument('--perf',action='store_true');a=ap.parse_args()
 out=(ROOT/'build/comparisons'/a.name).resolve()
 if not out.is_relative_to((ROOT/'build/comparisons').resolve()):ap.error('Invalid output folder')
 out.mkdir(parents=True,exist_ok=True)
 cfg=configuration();env=game_environment(cfg,disc_path(cfg))
 for k in list(env):
  if k.startswith(('MELEE_TEST_','MELEE_CAPTURE_','MELEE_NETPLAY_','MELEE_MOD_TEST_','GCN_GXT_')) or k=='MELEE_INPUT':env.pop(k)
 assets=ROOT/'build/akaneia-runtime-probe/data';dol=assets/'sys/main.dol';disc=ROOT/'upstream/akaneia-inspect/akaneia-1.0.1.iso'
 if a.content_root:
  content=(ROOT/a.content_root).resolve()
  if not content.is_relative_to((ROOT/'build/akaneia-content').resolve()):ap.error('Invalid content folder')
  audit=json.loads((content/'content-audit.json').read_text())
  if audit['scope']!=['fighters','stages','music']:raise ValueError('Unexpected content scope')
  disc=content/'yampp-content.iso';assets=out/'disc';(assets/'sys').mkdir(parents=True,exist_ok=True)
  with disc.open('rb') as f:
   f.seek(0x420);do,fo,fs=struct.unpack('>3I',f.read(12))
   f.seek(do);dh=f.read(0x100);ds=max(struct.unpack_from('>I',dh,i*4)[0]+struct.unpack_from('>I',dh,0x90+i*4)[0] for i in range(18))
   f.seek(do);(assets/'sys/main.dol').write_bytes(f.read(ds))
   f.seek(fo);(assets/'sys/fst.bin').write_bytes(f.read(fs))
  dol=assets/'sys/main.dol'
 elif hashlib.sha256(dol.read_bytes()).hexdigest()!='f604e1399f2dc88e2fbbaa2b3441867ca34d00b99b82e16bee4e4ef108604805':raise ValueError('Unexpected Akaneia DOL')
 settings=out/'settings.xml';settings.write_text('<melee-settings schema="1" width="960" height="720" renderScale="1" widescreen="0" fullscreen="0" vsync="0" mute="1" />')
 if not (out/'card.raw').exists():
  seed=(ROOT/'build/comparisons'/a.seed_from).resolve() if a.seed_from else None
  if seed and not seed.is_relative_to((ROOT/'build/comparisons').resolve()):ap.error('Invalid seed folder')
  shutil.copy2(seed/'card.raw' if seed else ROOT/'build/netplay-work/template-card.raw',out/'card.raw')
  if seed and (seed/'cache').is_dir():shutil.copytree(seed/'cache',out/'cache',dirs_exist_ok=True)
 env.update(MELEE_DISC=str(disc),MELEE_FST=str(assets/'sys/fst.bin'),MELEE_MEX_BASE_DOL=str(ROOT/'data/GALE01/sys/main.dol'),MELEE_UNICORN_LIBRARY=str(ROOT/'build/pc/bin/unicorn.dll'),MELEE_AURORA_DLL=str(ROOT/'build/pc/bin/melee_aurora_controllers.dll'),MELEE_SETTINGS=str(settings),MELEE_MEMORY_CARD=str(out/'card.raw'),MELEE_COSTUME_REGISTRY=str(out/'empty-costumes.tsv'),MELEE_MOD_REGISTRY=str(out/'empty-registry.tsv'),GCN_AURORA_HIDDEN='1',GCN_AURORA_DATA_DIR=str(out/'cache'),MELEE_TEST_FAST_EXIT='1',MELEE_CAPTURE_AURORA='1',MELEE_CAPTURE_UI='1',MELEE_CAPTURE_INTERVAL='10',MELEE_CAPTURE_DIR=str(out),MELEE_INPUT='300:100:12,600:100:12,900:1000:12,1200:1000:12,1600:100:12,1900:100:12')
 if os.environ.get('MELEE_PROBE_COSTUMES'):env['MELEE_COSTUME_REGISTRY']=os.environ['MELEE_PROBE_COSTUMES']
 if a.input:env['MELEE_INPUT']=a.input
 if a.stage is not None:env['MELEE_TEST_STAGE']=str(a.stage)
 if a.menu:env['MELEE_TEST_MENU']=a.menu
 if a.audio:env['MELEE_AUDIO_CAPTURE']=str(out/'audio.pcm')
 if a.no_capture:
  for key in list(env):
   if key.startswith('MELEE_CAPTURE_'):env.pop(key)
 if a.perf:env.update(MELEE_PERF_CSV=str(out/'frames.csv'),MELEE_SAMPLE_START='2100',MELEE_SAMPLE_END=str(a.frames),MELEE_SAMPLE_TOP='128',MELEE_SAMPLE_PATH=str(out/'guest-prof.txt'))
 env['PATH']=str(ROOT/'build/pc/bin')+';C:/msys64/mingw64/bin;'+env['PATH']
 exe=Path(os.environ.get('MELEE_PROBE_EXE',str(ROOT/'build/native-game/YAMPP-akaneia-probe.exe')));started=time.monotonic()
 with (out/'run.log').open('w') as log:
  proc=subprocess.Popen([str(exe),str(dol),str(a.frames)],cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
  print('Owned Akaneia probe PID',proc.pid,flush=True)
  try:proc.wait(timeout=max(150,a.frames/60*5))
  except subprocess.TimeoutExpired:proc.terminate();proc.wait(10)
 text=(out/'run.log').read_text(errors='replace')
 report={'exit':proc.returncode,'seconds':time.monotonic()-started,'fault':'[fault]' in text,'assertions':re.findall(r'^.*assertion .*failed.*$',text,re.M),'captures':len(list(out.glob('frame_*.ppm'))),'dynamicCalls':re.findall(r'^\[mex-runtime\].*$',text,re.M)}
 (out/'report.json').write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2))
 if report['exit'] or report['fault'] or report['assertions']:raise SystemExit(1)
if __name__=='__main__':main()
