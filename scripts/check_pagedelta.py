"""Prove that incremental snapshots reproduce whole ones.

Rollback restores a state the simulation then continues from, so a page the
map failed to carry is not a slow frame -- it is a game that has silently
stopped agreeing with its opponent. The harness compares every operation
against a full-copy model.
"""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "checks"
OUT.mkdir(parents=True, exist_ok=True)
exe = OUT / ("pagedelta_test.exe" if sys.platform == "win32" else "pagedelta_test")
cc = Path("C:/msys64/mingw64/bin/gcc.exe") if sys.platform == "win32" else Path("cc")
# The compiler needs its own runtime DLLs alongside it.
os.environ["PATH"] = str(cc.parent) + os.pathsep + os.environ["PATH"]
subprocess.run([str(cc), "-O2", "-Wall", "-Wextra", "-Werror",
                str(ROOT / "native/host/gxrt/tests/pagedelta_test.c"),
                str(ROOT / "native/host/gxrt/pagedelta.c"),
                "-o", str(exe)], check=True, cwd=ROOT)
raise SystemExit(subprocess.run([str(exe)], cwd=ROOT).returncode)
