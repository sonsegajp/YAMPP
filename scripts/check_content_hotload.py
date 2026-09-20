"""Actual native menu toggles; assert one process/window across content reloads."""
import argparse,json,os,re,shutil,subprocess,sys
from pathlib import Path
from project_config import ROOT

def main():
 ap=argparse.ArgumentParser();ap.add_argument("--name",default="content-hotload-cycle");ap.add_argument("--cycles",type=int,default=2);ap.add_argument("--fail-prepare",action="store_true");args=ap.parse_args()
 if args.fail_prepare:args.cycles=1
 if not 1<=args.cycles<=8:ap.error("cycles must be 1..8")
 out=(ROOT/"build/comparisons"/args.name).resolve()
 if not out.is_relative_to(ROOT/"build/comparisons"):ap.error("invalid output")
 out.mkdir(parents=True,exist_ok=True)
 state=out/"content-state.json";record=json.loads((ROOT/"user/content-mods.json").read_text())["akaneia"].copy();record["enabled"]=True;state.write_text(json.dumps({"schema":1,"akaneia":record}))
 cases=[{"MELEE_INPUT":"1800:100:8" if n<args.cycles else "","MELEE_TEST_MENU":"","MELEE_CAPTURE_DIR":str(out/("generation-%d"%(n+1)))} for n in range(args.cycles+1)]
 if args.fail_prepare:cases[1]["INVALID_TEST_KEY"]="reject"
 script=out/"generations.json";script.write_text(json.dumps({"generations":cases}))
 env={"MELEE_CONTENT_STATE":str(state),"MELEE_CONTENT_RETURN":"1","MELEE_CONTENT_TEST_PLAN":str(script),"MELEE_CONTENT_GENERATION":"1"}
 plan={"directory":out.relative_to(ROOT).as_posix(),"executable":"build/native-game/YAMPP-hotload-test.exe","renderer":"build/pc/bin/melee_aurora_hotload.dll","timeout":180*(args.cycles+1),"launches":[{"frames":2400,"environment":env}]}
 path=out/"plan.json";path.write_text(json.dumps(plan,indent=2))
 (out/"settings.xml").write_text('<melee-settings schema="1" width="960" height="720" widescreen="0" fullscreen="0" vsync="0" mute="1" />')
 shutil.copy2(ROOT/"build/akaneia-content-user-test/card.raw",out/"card.raw")
 environment=os.environ.copy();environment.update(env)
 with (out/"launcher.log").open("w") as log:
  code=subprocess.run([sys.executable,str(ROOT/"scripts/open_managed_test.py"),"--test-plan",str(path)],cwd=ROOT,env=environment,stdout=log,stderr=log,timeout=plan["timeout"]+30,creationflags=subprocess.CREATE_NO_WINDOW).returncode
 record=json.loads((out/"launch.json").read_text());text=Path(record["log"]).read_text(errors="replace")
 boots=re.findall(r"\[content-host\] generation=(\d+) pid=(\d+) enabled=(\d+)",text)
 windows=re.findall(r"\[content-host\] window=(\d+) generation=(\d+)",text)
 report={"launcherExit":code,"generations":boots,"windows":windows,"sameProcess":len({p for _,p,_ in boots})==1,"sameWindow":len({w for w,_ in windows})==1 and bool(windows) and int(windows[0][0])!=0,"fault":"[fault]" in text or "[content-host-fault]" in text,"assertion":bool(re.search(r"assertion .*failed",text)),"exit":json.loads((out/"exit-1.json").read_text())["code"] if (out/"exit-1.json").exists() else None}
 report["passed"]=code==0 and report["exit"]==0 and len(boots)==len(windows)==args.cycles+1 and [int(e) for _,_,e in boots]==([1,1] if args.fail_prepare else [int(n%2==0) for n in range(args.cycles+1)]) and report["sameProcess"] and report["sameWindow"] and not report["fault"] and not report["assertion"]
 if args.fail_prepare:
  report["recovered"]="Preparation failed; restoring previous content" in text and "Previous setup restored" in text and json.loads(state.read_text())["akaneia"]["enabled"] is True
  report["passed"]=report["passed"] and report["recovered"]
 (out/"report.json").write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2));return 0 if report["passed"] else 1
if __name__=="__main__":raise SystemExit(main())
