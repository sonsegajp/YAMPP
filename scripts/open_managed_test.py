"""Launch YAMPP with content mods, automatically reload after menu toggles."""
import argparse,json,os,shutil,subprocess,sys,time
from pathlib import Path
from project_config import ROOT,configuration,disc_path,game_environment,project_path
sys.path.insert(0,str(ROOT/"tools/modkit"))
from content_mods import read_state,checked_content,boot_files,register

def main():
    ap=argparse.ArgumentParser();ap.add_argument("--register",type=Path);ap.add_argument("--enable",action="store_true")
    ap.add_argument("--worker",action="store_true");ap.add_argument("--prepare-only",action="store_true")
    ap.add_argument("--hot-reload",action="store_true")
    ap.add_argument("--test-plan",type=Path,help="Isolated developer replay plan under build/")
    args=ap.parse_args();test=None
    if args.test_plan:
        path=args.test_plan.resolve()
        if not path.is_relative_to(ROOT/"build"):raise ValueError("Test plan must be under build/")
        test=json.loads(path.read_text());args.worker=True
    out=(ROOT/test["directory"]).resolve() if test else ROOT/"build/managed-user-test"
    if not out.is_relative_to(ROOT/"build"):raise ValueError("Invalid launcher output")
    out.mkdir(parents=True,exist_ok=True)
    if args.register:register(args.register,args.enable)
    if args.prepare_only:return
    if not args.worker:
        marker=out/"launch.json"
        if marker.exists():
            from open_akaneia_test import running_test
            old=json.loads(marker.read_text())
            if running_test(old.get("pid",0),Path(old["executable"])):
                print("The managed YAMPP test is already open.");return
        with (out/"launcher.log").open("w") as log:
            proc=subprocess.Popen([sys.executable,str(Path(__file__).resolve()),"--worker"]+(["--hot-reload"] if args.hot_reload else []),cwd=ROOT,stdout=log,stderr=log,creationflags=subprocess.CREATE_NO_WINDOW if os.name=="nt" else 0)
        print("Managed YAMPP launcher started:",proc.pid);return
    cfg=configuration();original=project_path(cfg["assets"]["directory"])/"sys/main.dol"
    exe=ROOT/"build/native-game/YAMPP-managed-test.exe";renderer=ROOT/"build/pc/bin/melee_aurora_content_manager.dll"
    if args.hot_reload:
        exe=ROOT/"build/native-game/YAMPP-hotload-test.exe";renderer=ROOT/"build/pc/bin/melee_aurora_hotload.dll"
    if test:
        exe=ROOT/test["executable"];renderer=ROOT/test["renderer"]
    for item in (exe,renderer):
        if not item.is_file():raise ValueError("Build the current candidate first")
    card=out/"card.raw";settings=out/"settings.xml"
    if not card.exists():shutil.copy2(ROOT/"build/akaneia-content-user-test/card.raw",card)
    if not settings.exists():shutil.copy2(ROOT/"build/akaneia-content-user-test/settings.xml",settings)
    resume=None;serial=0
    while True:
        state=read_state();record=state.get("akaneia");enabled=bool(record and record.get("enabled"))
        env=game_environment(cfg,disc_path(cfg))
        for key in list(env):
            if key.startswith(("MELEE_TEST_","MELEE_CAPTURE_","MELEE_NETPLAY_","MELEE_MOD_TEST_","MELEE_AUDIO_","MELEE_PERF_","MELEE_SAMPLE_","MELEE_TRACE_","GCN_GXT_","MELEE_MEX_","MELEE_CONTENT_")) or key in ("MELEE_INPUT","GCN_AURORA_HIDDEN","MELEE_RUNTIME_CORE","MELEE_RUNTIME_HOSTED"):env.pop(key)
        restart=out/"restart.json";restart.unlink(missing_ok=True)
        env.update(MELEE_VANILLA_BOOT="1",MELEE_AURORA_DLL=str(renderer),MELEE_MEMORY_CARD=str(card),MELEE_SETTINGS=str(settings),GCN_AURORA_DATA_DIR=str(out/"cache"),MELEE_CONTENT_LAUNCHER="1",MELEE_CONTENT_RESTART=str(restart),MELEE_WORKSHOP_PYTHON=sys.executable,MELEE_NETPLAY_SERVER="mmodx.fun/melee/")
        if test and os.environ.get("MELEE_CONTENT_STATE"):env["MELEE_CONTENT_STATE"]=os.environ["MELEE_CONTENT_STATE"]
        # Dynamic upstream content owns the expanded fighter/stage tables;
        # additive skin packages remain active through the shared costume loader.
        dol=original
        if enabled:
            disc=checked_content(record,True)
            if not disc:raise ValueError("Enabled content is missing")
            dol,fst=boot_files(disc,out/"disc/sys")
            env.update(MELEE_DISC=str(disc),MELEE_FST=str(fst),MELEE_MEX_BASE_DOL=str(original),MELEE_UNICORN_LIBRARY=str(ROOT/"build/pc/bin/unicorn.dll"),MELEE_MOD_REGISTRY=str(out/"no-legacy-replacements.tsv"))
        if resume:
            env["MELEE_CONTENT_RETURN"]="1"
            if resume.get("room",0)>0:
                env.update(MELEE_CONTENT_JOIN_ROOM=str(resume["room"]),MELEE_NETPLAY_SERVER=resume["server"],MELEE_NETPLAY_NAME=resume["name"])
        env["PATH"]=str(ROOT/"build/pc/bin")+os.pathsep+"C:/msys64/mingw64/bin"+os.pathsep+env["PATH"]
        serial+=1;log_path=out/("run-"+time.strftime("%Y%m%d-%H%M%S")+"-"+str(serial)+".log")
        frames="0"
        if test:
            case=test["launches"][serial-1];frames=str(case["frames"])
            capture=out/("launch-"+str(serial));capture.mkdir(exist_ok=True)
            env.update(GCN_AURORA_HIDDEN="1",MELEE_TEST_FAST_EXIT="1",MELEE_CAPTURE_AURORA="1",MELEE_CAPTURE_UI="1",MELEE_CAPTURE_INTERVAL="60",MELEE_CAPTURE_DIR=str(capture))
            env.update(case.get("environment",{}))
        if exe.with_suffix(".core.dll").is_file():
            plan=out/"boot-plan.env"
            env.update(MELEE_CONTENT_BOOT_PLAN=str(plan),MELEE_CONTENT_PLAN_SCRIPT=str(ROOT/"scripts/prepare_content_runtime.py"))
            subprocess.run([sys.executable,env["MELEE_CONTENT_PLAN_SCRIPT"],"--output",str(plan)],cwd=ROOT,env=env,check=True)
        with log_path.open("w") as log:
            proc=subprocess.Popen([str(exe),str(dol),frames],cwd=ROOT,env=env,stdout=log,stderr=log,creationflags=subprocess.CREATE_NO_WINDOW if os.name=="nt" else 0)
            (out/"launch.json").write_bytes(json.dumps({"pid":proc.pid,"executable":str(exe),"akaneia":enabled,"log":str(log_path)}).encode())
            try:code=proc.wait(timeout=min(1800,int(test.get("timeout",180))) if test else None)
            except subprocess.TimeoutExpired:proc.terminate();proc.wait(10);raise
        if test:(out/("exit-"+str(serial)+".json")).write_text(json.dumps({"code":code,"akaneia":enabled,"log":str(log_path)}))
        if code!=73:return code
        resume=json.loads(restart.read_text())
        if resume.get("schema")!=1 or type(resume.get("room")) is not int:raise ValueError("Invalid menu reload request")
        restart.unlink()
if __name__=="__main__":raise SystemExit(main())
