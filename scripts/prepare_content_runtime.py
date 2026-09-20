"""Prepare verified boot inputs for the persistent native host; never launches a game."""
import argparse,json,os,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from project_config import ROOT,configuration,disc_path,project_path
sys.path.insert(0,str(ROOT/"tools/modkit"))
from content_mods import read_state,save_state,checked_content,boot_files

def prepare(output,resume=False,recover=False):
 output=output.resolve()
 if not output.is_relative_to(ROOT/"build"):raise ValueError("Runtime plan must remain under build/")
 output.parent.mkdir(parents=True,exist_ok=True)
 if recover:
  previous=os.environ.get("MELEE_CONTENT_ENABLED")
  if previous not in ("0","1"):raise ValueError("Previous content profile is unavailable")
  state=read_state()
  if state.get("akaneia"):
   state["akaneia"]["enabled"]=previous=="1";save_state(state)
  os.environ.update(MELEE_CONTENT_RETURN="1",MELEE_CONTENT_JOIN_ROOM="",MELEE_INPUT="",MELEE_CONTENT_TEST_PLAN="")
 cfg=configuration();base=project_path(cfg["assets"]["directory"])/"sys/main.dol"
 record=read_state().get("akaneia");enabled=bool(record and record.get("enabled"))
 values={"MELEE_RUNTIME_DOL":str(base),"MELEE_FST":str(base.parent/"fst.bin"),"MELEE_DISC":str(disc_path(cfg)),"MELEE_MEX_BASE_DOL":"","MELEE_UNICORN_LIBRARY":"","MELEE_MOD_REGISTRY":str(project_path(cfg["user"]["mods"])/"registry.tsv"),"MELEE_CONTENT_RETURN":os.environ.get("MELEE_CONTENT_RETURN",""),"MELEE_CONTENT_JOIN_ROOM":os.environ.get("MELEE_CONTENT_JOIN_ROOM",""),"MELEE_CONTENT_ENABLED":str(int(enabled))}
 if enabled:
  disc=checked_content(record,True)
  if not disc:raise ValueError("Enabled content is missing")
  dol,fst=boot_files(disc,output.parent/"disc/sys")
  engine=ROOT/"bin/unicorn.dll" if (ROOT/"bin/unicorn.dll").is_file() else ROOT/"build/pc/bin/unicorn.dll"
  if not engine.is_file():raise ValueError("The content execution library is missing; restore this YAMPP build")
  values.update(MELEE_RUNTIME_DOL=str(dol),MELEE_FST=str(fst),MELEE_DISC=str(disc),MELEE_MEX_BASE_DOL=str(base),MELEE_UNICORN_LIBRARY=str(engine),MELEE_MOD_REGISTRY=str(output.parent/"no-legacy-replacements.tsv"))
 if resume:
  request=json.loads(Path(os.environ["MELEE_CONTENT_RESTART"]).read_text())
  if request.get("schema")!=1 or type(request.get("room"))is not int:raise ValueError("Invalid content request")
  values["MELEE_CONTENT_RETURN"]="1"
  values["MELEE_CONTENT_JOIN_ROOM"]=""
  if request["room"]>0:
   values.update(MELEE_CONTENT_JOIN_ROOM=str(request["room"]),MELEE_NETPLAY_SERVER=request["server"],MELEE_NETPLAY_NAME=request["name"])
 if recover:values["MELEE_INPUT"]=""
 test=os.environ.get("MELEE_CONTENT_TEST_PLAN")
 if test:
  path=Path(test).resolve()
  if not path.is_relative_to(ROOT/"build"):raise ValueError("Invalid developer content plan")
  cases=json.loads(path.read_text())["generations"];index=int(os.environ.get("MELEE_CONTENT_GENERATION","1"))-1
  if index>=len(cases):raise ValueError("Developer content plan exhausted")
  for key,value in cases[index].items():
   if key not in ("MELEE_INPUT","MELEE_TEST_MENU","MELEE_NETPLAY_AUTO","MELEE_NETPLAY_INPUT","MELEE_CAPTURE_DIR"):raise ValueError("Invalid test override")
   values[key]=value
  if values.get("MELEE_CAPTURE_DIR"):Path(values["MELEE_CAPTURE_DIR"]).mkdir(parents=True,exist_ok=True)
 # Test environments can isolate registries; the managed launcher supplies this
 # only for deliberate test plans. Normal play always uses the real registry.
 if os.environ.get("MELEE_CONTENT_TEST_MOD_REGISTRY"):
  values["MELEE_MOD_REGISTRY"]=os.environ["MELEE_CONTENT_TEST_MOD_REGISTRY"]
 for k,v in values.items():
  if any(c in str(v) for c in "\r\n\t\0"):raise ValueError("Invalid boot input")
 temporary=output.with_suffix(".tmp")
 temporary.write_text("".join(k+"\t"+v+"\n" for k,v in values.items()),encoding="utf-8")
 temporary.replace(output)
 return values

if __name__=="__main__":
 ap=argparse.ArgumentParser();ap.add_argument("--output",type=Path,required=True);mode=ap.add_mutually_exclusive_group();mode.add_argument("--resume",action="store_true");mode.add_argument("--recover",action="store_true");args=ap.parse_args()
 prepare(args.output,args.resume,args.recover)
