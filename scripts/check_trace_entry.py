"""Check entry-hook refactoring against its original ordered callbacks/state."""
import json,os,platform,subprocess
from pathlib import Path
from check_wgpipe import function
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/tests/trace-entry'
def main():
    OUT.mkdir(parents=True,exist_ok=True)
    s=(ROOT/'native/runtime/module_rt.c').read_text(encoding='utf-8')
    body=function(s,'static TRACE_COLD_NOINLINE void trace_service_arq(')+'\n'+function(s,'void trace_enter(')
    (OUT/'production.inc').write_text(body,encoding='utf-8')
    windows=platform.system()=='Windows';cc='C:/msys64/mingw64/bin/gcc.exe' if windows else os.environ.get('CC','gcc')
    env=os.environ.copy()
    if windows:env['PATH']=str(Path(cc).parent)+os.pathsep+env['PATH']
    exe=OUT/('trace-test.exe' if windows else 'trace-test')
    subprocess.run([cc,'-O2','-fno-strict-aliasing','-I'+str(ROOT/'native/runtime'),'-I'+str(ROOT/'native/vendor/include'),'-I'+str(OUT),str(ROOT/'native/host/gxrt/tests/trace_entry_test.c'),'-o',str(exe)],env=env,check=True)
    results=[]
    for diag in (False,True):
        e=env.copy();e.pop('MELEE_TRACE_DIAGNOSTICS',None)
        if diag:e['MELEE_TRACE_DIAGNOSTICS']='1'
        r=subprocess.run([str(exe)],env=e,text=True,capture_output=True)
        results.append({'diagnostics':diag,'passed':r.returncode==0,'output':r.stdout+r.stderr})
    report={'platform':platform.system(),'passed':all(x['passed'] for x in results),'runs':results}
    (OUT/('report-windows.json' if windows else 'report-linux.json')).write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2))
    if not report['passed']:raise SystemExit(1)
if __name__=='__main__':main()
