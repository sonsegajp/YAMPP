"""Exercise the production persistent-host parser and Windows argument quoting."""
import os,subprocess
from pathlib import Path
from project_config import ROOT
cc=Path("C:/msys64/mingw64/bin/gcc.exe")
out=ROOT/"build/tests/content-host";out.mkdir(parents=True,exist_ok=True)
exe=out/"test.exe"
os.environ["PATH"]=str(cc.parent)+os.pathsep+os.environ["PATH"]
subprocess.run([str(cc),"-O2",str(ROOT/"native/host/gxrt/tests/content_host_test.c"),"-o",str(exe),"-lshell32","-luser32"],check=True)
subprocess.run([str(exe),str(out/"plan.env")],check=True)
