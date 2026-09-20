"""Exercise native match -> results -> CSS transitions with real PAD input.
The victory case waits for the normal two-minute timer; it does not force an outcome.
"""
import argparse,hashlib,json,os,shutil,subprocess,sys
from pathlib import Path
from project_config import ROOT,configuration,project_path

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--case',choices=['no-contest','victory'],required=True)
    ap.add_argument('--name')
    ap.add_argument('--saved-card',type=Path,help='Copy this card into the isolated test directory')
    args=ap.parse_args();name=args.name or ('match-results-'+args.case)
    out=ROOT/'build/comparisons'/name;out.mkdir(parents=True,exist_ok=True)
    cfg=configuration();env=os.environ.copy()
    for key in list(env):
        if key.startswith(('MELEE_TEST_','MELEE_MOD_TEST_','MELEE_TRACE_','MELEE_RENDERDOC_')) or key in ('MELEE_MEMORY_CARD','MELEE_SETTINGS'):env.pop(key)
    env['MELEE_MODS']='1'
    env['MELEE_AURORA_DLL']=str(project_path(cfg['runtime']['developmentRenderer']))
    env['MELEE_MOD_REGISTRY']=str(project_path(cfg['user']['mods'])/'registry.tsv')
    if args.saved_card:
        card=out/'card.raw';shutil.copy2(args.saved_card,card);env['MELEE_MEMORY_CARD']=str(card)
        boot='300:100:8,420:100:8,600:1000:8,900:4:8,990:100:8,1080:100:8'
    else:
        boot='300:100:8,420:100:8,600:1000:8,840:1000:8,1020:4:8,1110:100:8,1200:100:8'
    # P1 places its token on the separate bottom-left custom Falcon tile.
    select='1600:0:22:0:80,1700:100:8'
    stage='2000:1000:8,2190:0:20:0:80,2230:0:20:80:0,2310:100:8'
    if args.case=='no-contest':
        select+=',1770:0:11:80:0,1800:0:7:0:-80,1860:100:8' # enable CPU
        end='3000:1000:8,3060:1160:10,3930:1000:8,4290:1000:8,4800:1000:8,5100:0:20:0:80,5140:0:20:80:0,5240:100:8,6300:1000:8,6360:1160:10,7230:1000:8,7590:1000:8'
        frames=8040;expected_results=2
    else:
        # P2 selects original Falcon and loses points by walking off the stage.
        select+=',1600:0:33:0:80:1,1650:0:27:80:0:1,1740:100:8:0:0:1'
        end='2750:0:500:80:0:1,3800:0:500:80:0:1,5000:0:500:80:0:1,10020:1000:8,10260:1000:8,10500:1000:8,10260:1000:8:0:0:1,10500:1000:8:0:0:1'
        frames=10920;expected_results=1
    timeline=','.join((boot,select,stage,end))
    result=subprocess.run([sys.executable,str(ROOT/'scripts/run_native_check.py'),'--name',name,'--frames',str(frames),'--executable','melee-mod.exe','--normal-exit','--capture','--timeline',timeline],cwd=ROOT,env=env)
    report=json.loads((out/'report.json').read_text());log=(out/'run.log').read_text(errors='replace')
    report['results_entries']=log.count('[mods] results enter;')
    report['results_exits']=log.count('[mods] results exit')
    report['css_entries']=log.count('[mods] CSS enter arg=')
    report['custom_costume_loads']=log.count('[mods] P1 independent costume=')
    report['executable_sha256']=hashlib.sha256(project_path(cfg['runtime']['developmentExecutable']).read_bytes()).hexdigest()
    report['passed']=bool(report['passed'] and result.returncode==0 and report['results_entries']>=expected_results and report['results_exits']>=expected_results and report['css_entries']>=expected_results+1 and report['custom_costume_loads']>=expected_results*2)
    (out/'transition-report.json').write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2))
    if not report['passed']:raise SystemExit(1)
if __name__=='__main__':main()
