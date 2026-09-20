"""Native consent -> managed relaunch -> correct room, against a local fixture.

The host is a protocol fixture; actual two-game rollback is checked separately
by check_netplay_local.py. All settings and mod state here are isolated.
"""
import argparse,json,os,re,shutil,socket,subprocess,sys,time
from pathlib import Path
from unittest.mock import patch
from project_config import ROOT,configuration,disc_path,project_path
from check_netplay_ui import Fixture
sys.path.insert(0,str(ROOT/"tools/modkit"));sys.path.insert(0,str(ROOT/"server"))
from compatibility import fingerprint,plan
from content_mods import boot_files
from upstream_builds import BUILDS,identity


def main():
    ap=argparse.ArgumentParser();ap.add_argument("--target",choices=["akaneia","stock"],default="akaneia");ap.add_argument("--hot-reload",action="store_true");args=ap.parse_args()
    wanted=args.target=="akaneia"
    out=ROOT/"build/comparisons"/(("content-hotjoin-" if args.hot_reload else "content-rejoin-")+args.target);out.mkdir(parents=True,exist_ok=True)
    state=ROOT/"build/modkit"/("rejoin-"+args.target)/"state.json";state.parent.mkdir(parents=True,exist_ok=True)
    record=json.loads((ROOT/"user/content-mods.json").read_text())["akaneia"].copy();record["enabled"]=not wanted
    state.write_text(json.dumps({"schema":1,"akaneia":record}))
    mods=state.parent/"mods";mods.mkdir(exist_ok=True)
    exe=ROOT/("build/native-game/YAMPP-hotload-test.exe" if args.hot_reload else "build/native-game/YAMPP-managed-test.exe")
    cfg=configuration();base=project_path(cfg["assets"]["directory"])/"sys/main.dol"
    content=ROOT/record["directory"]/"yampp-content.iso";dol,fst=boot_files(content,out/"target/sys")
    target={"MELEE_RUNTIME_DOL":str(dol if wanted else base),"MELEE_FST":str(fst if wanted else base.parent/"fst.bin"),"MELEE_DISC":str(content if wanted else disc_path(cfg))}
    if wanted:target.update(MELEE_MEX_BASE_DOL=str(base),MELEE_UNICORN_LIBRARY=str(ROOT/"build/pc/bin/unicorn.dll"))
    if args.hot_reload:target["MELEE_RUNTIME_CORE"]=str(exe.with_suffix(".core.dll"))
    with patch.dict(os.environ,target):manifest=fingerprint(plan(exe))
    compat={"schema":1,"fingerprint":manifest["aspect4x3"],"runtime":manifest["runtimeIdentity4x3"],"game":manifest["game"]}
    with socket.socket() as sock:sock.bind(("127.0.0.1",0));port=sock.getsockname()[1]
    serverlog=(out/"server.log").open("w")
    server=subprocess.Popen([sys.executable,str(ROOT/"server/melee_netplay_server.py"),"--bind","127.0.0.1","--port",str(port)],cwd=ROOT,stdout=serverlog,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
    host=None
    try:
        deadline=time.monotonic()+10
        while True:
            try:
                with socket.create_connection(("127.0.0.1",port),.1):break
            except OSError:
                if time.monotonic()>deadline:raise RuntimeError("Local relay failed")
                time.sleep(.05)
        host=Fixture(port,"Content Host","Content Reload Test",compatibility=compat,upstream_build=identity(BUILDS[0]) if wanted else None)
        while not host.room and time.monotonic()<deadline:time.sleep(.05)
        assert host.room,"Fixture did not create its room"
        room=host.room
        common={"MELEE_CONTENT_STATE":str(state),"MELEE_WORKSHOP_TEST_MODS":str(mods),"MELEE_COSTUME_REGISTRY":str(mods/"costumes.tsv"),"MELEE_MOD_REGISTRY":str(mods/"registry.tsv"),"MELEE_MOD_REPOSITORY_URL":"http://127.0.0.1:%d/api/mods"%port,"MELEE_NETPLAY_SERVER":"127.0.0.1:%d"%port,"MELEE_NETPLAY_NAME":"Reload Tester"}
        first=dict(common,MELEE_CONTENT_RETURN="1",MELEE_CONTENT_JOIN_ROOM=str(room),MELEE_INPUT="1500:100:8,2100:100:8")
        second=dict(common,MELEE_INPUT="")
        spec={"directory":out.relative_to(ROOT).as_posix(),"executable":exe.relative_to(ROOT).as_posix(),"renderer":"build/pc/bin/melee_aurora_controllers.dll","launches":[{"frames":3000,"environment":first},{"frames":1800,"environment":second}]}
        if args.hot_reload:
            generations=out/"generations.json"
            generations.write_text(json.dumps({"generations":[{"MELEE_INPUT":first["MELEE_INPUT"],"MELEE_CAPTURE_DIR":str(out/"generation-1")},{"MELEE_INPUT":"","MELEE_CAPTURE_DIR":str(out/"generation-2")}]}))
            first.update(MELEE_CONTENT_TEST_PLAN=str(generations),MELEE_CONTENT_GENERATION="1",MELEE_CONTENT_TEST_MOD_REGISTRY=str(mods/"registry.tsv"))
            spec.update(renderer="build/pc/bin/melee_aurora_hotload.dll",timeout=200,launches=[{"frames":3000,"environment":first}])
        path=out/"plan.json";path.write_text(json.dumps(spec,indent=2))
        (out/"settings.xml").write_text('<melee-settings schema="1" width="960" height="720" widescreen="0" vsync="0" mute="1" />')
        shutil.copy2(ROOT/"build/akaneia-content-user-test/card.raw",out/"card.raw")
        for n in (1,2):(out/("exit-%d.json"%n)).unlink(missing_ok=True)
        env=os.environ.copy();env["MELEE_CONTENT_STATE"]=str(state)
        with (out/"launcher.log").open("w") as log:
            process=subprocess.run([sys.executable,str(ROOT/"scripts/open_managed_test.py"),"--test-plan",str(path)],cwd=ROOT,env=env,stdout=log,stderr=log,timeout=220,creationflags=subprocess.CREATE_NO_WINDOW)
        exits=[json.loads((out/("exit-%d.json"%n)).read_text()) if (out/("exit-%d.json"%n)).exists() else {} for n in (1,2)]
        texts=[Path(e["log"]).read_text(errors="replace") if e else "" for e in exits]
        joined=any(m.get("op")=="room" and (m.get("room") or {}).get("id")==room and any(p.get("name")=="Reload Tester" for p in (m.get("room") or {}).get("players",[])) for m in host.messages)
        report={"target":args.target,"syntheticHost":True,"gameplayProof":False,"exitCodes":[e.get("code") for e in exits],"contentStates":[e.get("akaneia") for e in exits],"joinedCorrectRoom":joined,"remainedConnected":"Server connection closed" not in texts[1] and "menu kind 11 opened" in texts[1],"noFault":not any("[fault]" in t or re.search(r"assertion .*failed",t) for t in texts),"consentOffered":("This room uses Akaneia" if wanted else "original Melee") in texts[0],"launcherExit":process.returncode}
        report["passed"]=process.returncode==0 and report["exitCodes"]==[73,0] and report["contentStates"]==[not wanted,wanted] and joined and report["remainedConnected"] and report["noFault"] and report["consentOffered"]
        if args.hot_reload:
            combined=texts[0]
            boots=re.findall(r"\[content-host\] generation=(\d+) pid=(\d+) enabled=(\d+)",combined)
            windows=re.findall(r"\[content-host\] window=(\d+) generation=(\d+)",combined)
            final=combined.split("[content-host] generation=2 pid=")[-1]
            report.update(exitCodes=[exits[0].get("code")],contentStates=[bool(int(e)) for _,_,e in boots],
                sameProcess=len(boots)==2 and len({p for _,p,_ in boots})==1,
                sameWindow=len(windows)==2 and len({w for w,_ in windows})==1 and int(windows[0][0])!=0,
                remainedConnected="Server connection closed" not in final and "menu kind 11 opened" in final)
            report["passed"]=process.returncode==0 and report["exitCodes"]==[0] and report["contentStates"]==[not wanted,wanted] and joined and report["remainedConnected"] and report["noFault"] and report["consentOffered"] and report["sameProcess"] and report["sameWindow"]
        (out/"report.json").write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2))
        if not report["passed"]:raise SystemExit(1)
    finally:
        if host:host.close()
        server.terminate();server.wait(10);serverlog.close()

if __name__=="__main__":main()
