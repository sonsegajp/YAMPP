"""Encode a prepared costume portrait/icon as a verified native GX sidecar.

Pixels stay in the mod package; this module contains no game or mod artwork.
The input must already have its final 136x188 or 24x24 transparent canvas.
"""
import argparse
import base64
import json
from pathlib import Path
import struct
import subprocess
import tempfile

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
MAGIC = b"YAMPPTEX"


def encode(source, destination):
    image = Image.open(source).convert("RGBA")
    if image.size not in ((136, 188), (24, 24)):
        raise ValueError("Use a 136x188 portrait or 24x24 stock icon canvas")
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="yampp-texture-") as work:
        patch = Path(work)/"pixels.json"
        patch.write_text(json.dumps({"width": image.width, "height": image.height,
            "rgba": base64.b64encode(image.tobytes()).decode("ascii")}))
        subprocess.run([str(ROOT/"build/modkit/bridge/ModBridge.exe"),
            "native-texture", str(patch), str(destination)], check=True,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    raw = destination.read_bytes()
    width, height, fmt, size = struct.unpack_from(">4I", raw, 8)
    expected = ((width+7)//8)*((height+7)//8)*32
    if raw[:8] != MAGIC or (width, height) != image.size or fmt != 14 or size != expected or len(raw) != 24+size:
        raise ValueError("Native texture encoder returned an invalid sidecar")
    return destination


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    print(encode(args.source, args.destination))
