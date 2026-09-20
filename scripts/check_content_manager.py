"""Real UI reload smoke: stock boot returns to Mod Manager and enables Akaneia."""
import json,os,shutil,subprocess,sys,time
from pathlib import Path
from project_config import ROOT,configuration,disc_path,game_environment,project_path
out=ROOT/"build/comparisons/manager-enable-akaneia";out.mkdir(parents=True,exist_ok=True)
state=ROOT/"build/modkit/manager-cycle/state.json"
assert json.loads(state.read_text())["akaneia"]["enabled"] is False
cfg=configuration();env=game_environment(cfg,disc_path(cfg))
for key in list(env):
 if key.startswith(("MELEE_TEST_","MELEE_MEX_","MELEE_CAPTURE_","MELEE_NETPLAY_","MELEE_MOD_TEST_")):env.pop(key)
shutil.copy2(ROOT/"build/akaneia-content-user-test/card.raw",out/"card.raw")
(out/"settings.xml").write_text('<melee-settings schema="1" width="960" height="720" widescreen="0" vsync="0" mute="1" />')
restart=out/"restart.json";restart.unlink(missing_ok=True)
env.update(MELEE_CONTENT_STATE=str(state),MELEE_CONTENT_RESTART=str(restart),MELEE_CONTENT_LAUNCHER="1",MELEE_CONTENT_RETURN="1",MELEE_NETPLAY_SERVER="mmodx.fun/melee/",MELEE_MOD_REGISTRY=str(out/"empty-registry.tsv"),MELEE_MEMORY_CARD=str(out/"card.raw"),MELEE_SETTINGS=str(out/"settings.xml"),GCN_AURORA_DATA_DIR=str(out/"cache"),GCN_AURORA_HIDDEN="1",MELEE_AURORA_DLL=str(ROOT/"build/pc/bin/melee_aurora_controllers.dll"),MELEE_CAPTURE_AURORA="1",MELEE_CAPTURE_UI="1",MELEE_CAPTURE_INTERVAL="60",MELEE_CAPTURE_DIR=str(out),MELEE_TEST_FAST_EXIT="1",MELEE_INPUT="2100:100:8")
env["PATH"]=str(ROOT/"build/pc/bin")+";C:/msys64/mingw64/bin;"+env["PATH"]
with (out/"run.log").open("w") as log:
 p=subprocess.Popen([str(ROOT/"build/native-game/YAMPP-content-manager-final.exe"),str(project_path(cfg["assets"]["directory"])/"sys/main.dol"),"3200"],cwd=ROOT,env=env,stdout=log,stderr=log,creationflags=subprocess.CREATE_NO_WINDOW)
 print("Owned manager reload PID",p.pid,flush=True);code=p.wait(timeout=150)
text=(out/"run.log").read_text(errors="replace")
result={"exit":code,"akaneiaEnabled":json.loads(state.read_text())["akaneia"]["enabled"],"restart":restart.exists(),"fault":"[fault]" in text,"assertion":"assertion" in text.lower()}
result["passed"]=code==73 and result["akaneiaEnabled"] and result["restart"] and not result["fault"] and not result["assertion"]
(out/"report.json").write_text(json.dumps(result,indent=2));print(json.dumps(result))
raise SystemExit(0 if result["passed"] else 1)
