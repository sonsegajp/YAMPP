"""Run real Workshop UI against an isolated local production mod repository."""
from pathlib import Path
from datetime import datetime
import json
import os
import socket
import subprocess
import sys
import time
import urllib.request

ROOT=Path(__file__).resolve().parents[1]
WORK=ROOT/'build/modkit/electron-community-check'/datetime.now().strftime('%Y%m%d-%H%M%S')
WORK.mkdir(parents=True,exist_ok=True)
with socket.socket() as probe:
    probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
env=dict(os.environ)
env.pop('ELECTRON_RUN_AS_NODE',None)
env['MELEE_MOD_UPLOAD_TOKEN']='isolated-workshop-community-check'
env['MELEE_MOD_REPOSITORY_URL']='http://127.0.0.1:%d/api/mods'%port
env['MELEE_WORKSHOP_COMMUNITY_CHECK_DIR']=str(WORK)
env['MELEE_WORKSHOP_PYTHON']=sys.executable
flags=getattr(subprocess,'CREATE_NO_WINDOW',0)
server_log=(WORK/'server.log').open('w',encoding='utf-8')
ui_log=(WORK/'workshop.log').open('w',encoding='utf-8')
server=subprocess.Popen([sys.executable,'-u',str(ROOT/'server/melee_netplay_server.py'),'--bind','127.0.0.1','--port',str(port),'--mods-dir',str(WORK/'repository'),'--mod-api','http://127.0.0.1:%d/api'%port],cwd=ROOT,env=env,stdout=server_log,stderr=subprocess.STDOUT,creationflags=flags)
ui=None
try:
    for attempt in range(100):
        if server.poll() is not None:raise RuntimeError('Local repository exited; inspect '+str(WORK/'server.log'))
        try:
            with urllib.request.urlopen(env['MELEE_MOD_REPOSITORY_URL'],timeout=1) as response:
                assert json.load(response)['schema']==1
            break
        except OSError:time.sleep(.1)
    else:raise RuntimeError('Local repository did not become ready')
    desktop=ROOT/'tools/modkit/desktop'
    ui=subprocess.Popen([str(desktop/'node_modules/electron/dist/electron.exe'),str(desktop),'--workshop-community-check'],cwd=ROOT,env=env,stdout=ui_log,stderr=subprocess.STDOUT,creationflags=flags)
    code=ui.wait(timeout=360)
    print('Workshop community artifacts:',WORK)
    if code:
        failure=WORK/'failure.txt'
        print(failure.read_text() if failure.exists() else (WORK/'workshop.log').read_text())
        raise SystemExit(code)
    report=json.loads((WORK/'report.json').read_text())
    print(json.dumps(report,indent=2))
finally:
    if ui is not None and ui.poll() is None:
        if os.name=='nt':subprocess.run(['taskkill','/PID',str(ui.pid),'/T','/F'],capture_output=True,creationflags=flags)
        else:ui.terminate()
    server.terminate();server.wait(timeout=10)
    server_log.close();ui_log.close()
