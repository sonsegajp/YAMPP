"""Run the focused native costume loader and original-roster parity checks."""
from pathlib import Path
import os
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
compiler = os.environ.get("MELEE_GCC") or shutil.which("gcc") or r"C:\msys64\mingw64\bin\gcc.exe"
if not Path(compiler).is_file():
    raise SystemExit("Set MELEE_GCC to the MinGW gcc executable")
output = ROOT / "build/tests/costumes-test.exe"
output.parent.mkdir(parents=True, exist_ok=True)
environment = dict(os.environ)
environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
subprocess.run([compiler, "-O2", "-I", "native/runtime", "-I", "native/vendor/include",
                "-I", "build/native-game/generated", "native/host/gxrt/tests/costumes_test.c",
                "-o", str(output)], cwd=ROOT, env=environment, check=True)
subprocess.run([str(output)], cwd=ROOT, env=environment, check=True)
subprocess.run([sys.executable, "tools/modkit/test_costume_catalog.py"], cwd=ROOT, check=True)
