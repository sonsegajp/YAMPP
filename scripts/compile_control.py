"""Compile the exact DrawRectangle/DrawASCII source slice for renderer validation."""
from pathlib import Path
import argparse
import subprocess
from audit_native import ROOT

parser = argparse.ArgumentParser()
parser.add_argument("--clang", required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
args.output.parent.mkdir(parents=True, exist_ok=True)
command = [
    args.clang, "-c", "-O2", "-std=gnu99", "-fms-extensions",
    "-ffunction-sections", "-fdata-sections", "-ffp-contract=off", "-fno-strict-aliasing",
    "-D_CRT_SECURE_NO_WARNINGS", "-D_USE_MATH_DEFINES", "-DTARGET_PC=1", "-DAURORA=1",
    "-Wno-microsoft", "-Wno-unknown-pragmas",
    "-Wno-incompatible-function-pointer-types", "-Wno-incompatible-pointer-types",
    "-Wno-int-conversion", "-Wno-deprecated-non-prototype",
    "-include", str(ROOT / "src/platform/native_prelude.h"),
]
for folder in ("src/platform/include", "build/generated", "upstream/melee/src",
               "upstream/aurora/include", "upstream/melee/extern/dolphin/include"):
    command.extend(["-I", str(ROOT / folder)])
# This prefix contains the two original drawing functions and their static font
# data, unchanged. Keep unrelated camera/particle services out of this control.
original = ROOT / "upstream/melee/src/sysdolphin/baselib/hsd_3915.c"
source = original.read_text()
marker = "static u8 lbl_804D6078 = 12;"
if source.count(marker) != 1:
    raise SystemExit("Upstream drawing boundary changed; review the source slice")
control = args.output.with_suffix(".c")
control.write_text(source.split(marker)[0])
command.extend(["-I", str(original.parent)])
command.extend([str(control),
                "-o", str(args.output)])
raise SystemExit(subprocess.run(command).returncode)
