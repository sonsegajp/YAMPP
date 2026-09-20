"""Reject blank/clear-only captures; save a convenient PNG alongside the PPM."""
import argparse
import json
from pathlib import Path
from PIL import Image

parser = argparse.ArgumentParser()
parser.add_argument("capture", type=Path)
args = parser.parse_args()
with Image.open(args.capture) as image:
    image = image.convert("RGB")
    colors = image.getcolors(image.width * image.height)
    colorful = sum(n for n, rgb in colors if max(rgb) - min(rgb) > 50)
    result = {"width": image.width, "height": image.height,
              "unique_colors": len(colors), "colorful_pixels": colorful}
    image.save(args.capture.with_suffix(".png"))
    if len(colors) < 1000 or colorful < image.width * image.height // 10:
        raise SystemExit("FAIL: renderer capture lacks expected geometry: " + json.dumps(result))
    # The upper strip contains the original HSD line-font output.
    text_pixels = sum(n for n, rgb in image.crop((80, 25, 600, 55)).getcolors(520 * 30)
                      if min(rgb) > 180 and max(rgb) - min(rgb) < 35)
    result["hsd_text_pixels"] = text_pixels
    if text_pixels < 400:
        raise SystemExit("FAIL: original HSD text is missing: " + json.dumps(result))
    print(json.dumps(result, indent=2))
