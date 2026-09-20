"""Author and rasterize flat, hardware-shaped GameCube UI button glyphs.

All artwork and lettering is SVG geometry. PNG generation uses resvg, so exports
do not depend on an installed font or a web browser. This is vector artwork, not
an image-generation postprocessor. Run from any working directory.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets" / "ui" / "gamecube"
INK = "#172132"
EDGE = "#F0EAD6"
PALETTE = {"a": "#32B895", "b": "#DF4049", "gray": "#BBC0CA", "z": "#8175C8"}

# Upright line-letter geometry; no text nodes or third-party font are embedded.
LETTERS = {
    "A": "M -9 12 L 0 -12 L 9 12 M -5 3 H 5",
    "B": "M -7 -12 V 12 H 1 C 12 12 12 0 1 0 H -7 M -7 -12 H 0 C 11 -12 11 0 0 0",
    "X": "M -8 -12 L 8 12 M 8 -12 L -8 12",
    "Y": "M -8 -12 L 0 0 L 8 -12 M 0 0 V 12",
    "Z": "M -8 -12 H 8 L -8 12 H 8",
    "L": "M -7 -12 V 12 H 8",
    "R": "M -7 12 V -12 H 0 C 13 -12 13 0 0 0 H -7 M 0 0 L 10 12",
}

# Reference: original Nintendo GameCube controller, facing the player.
# These are authored UI silhouettes, not pixel traces or official CAD dimensions.
GLYPHS = [
    dict(name="a", label="A", color=PALETTE["a"],
         shape='<circle cx="64" cy="64" r="48"/>', center=[64,64], letter_scale=1.55,
         description="Large green round face button"),
    dict(name="b", label="B", color=PALETTE["b"],
         shape='<circle cx="64" cy="64" r="34"/>', center=[64,64], letter_scale=1.25,
         description="Smaller red round face button; diameter 71 percent of A"),
    dict(name="x", label="X", color=PALETTE["gray"],
         shape='<path d="M 56 17 C 71 12 86 26 92 45 C 99 65 101 88 91 103 C 84 115 69 115 62 104 C 55 94 61 84 59 72 C 58 59 48 50 46 38 C 43 28 47 20 56 17 Z"/>',
         center=[73,64], letter_scale=1.28,
         description="Tall gray X; inner edge faces left toward A, top leans left"),
    dict(name="y", label="Y", color=PALETTE["gray"],
         shape='<path d="M 17 55 C 27 41 47 32 70 29 C 91 26 109 31 113 45 C 117 57 106 66 92 65 C 79 63 67 63 56 69 C 43 77 28 83 20 75 C 14 70 13 62 17 55 Z"/>',
         center=[65,49], letter_scale=1.13,
         description="Wide gray Y above A; outer edge rises toward the right"),
    dict(name="z", label="Z", color=PALETTE["z"],
         shape='<path d="M 21 39 C 42 32 78 30 101 35 C 109 37 113 43 112 51 L 108 76 C 107 83 101 87 92 87 H 30 C 20 87 15 82 15 74 V 51 C 15 45 17 41 21 39 Z"/>',
         center=[64,60], letter_scale=1.28,
         description="Purple narrow shoulder button"),
    dict(name="l", label="L", color=PALETTE["gray"],
         shape='<path d="M 20 42 C 25 31 41 25 67 26 C 89 27 103 33 109 44 L 104 83 C 103 91 96 95 87 93 L 31 85 C 20 84 15 79 15 72 Z"/>',
         center=[62,58], letter_scale=1.38,
         description="Gray left analog trigger, broad curved top and tapered base"),
    dict(name="r", label="R", color=PALETTE["gray"],
         shape='<path d="M 108 42 C 103 31 87 25 61 26 C 39 27 25 33 19 44 L 24 83 C 25 91 32 95 41 93 L 97 85 C 108 84 113 79 113 72 Z"/>',
         center=[66,58], letter_scale=1.38,
         description="Gray right analog trigger, mirrored left trigger outline"),
    dict(name="start-pause", label="", color=PALETTE["gray"],
         shape='<circle cx="64" cy="64" r="24"/>', center=[64,64], letter_scale=1.0,
         description="Small blank gray Start/Pause button; use an adjacent text label"),
]


def group(glyph: dict) -> str:
    result = [f'<g fill="{glyph["color"]}" stroke="{INK}" stroke-width="9" stroke-linejoin="round">{glyph["shape"]}</g>',
              f'<g fill="none" stroke="{EDGE}" stroke-width="2.6" stroke-linejoin="round">{glyph["shape"]}</g>']
    if glyph["label"]:
        x, y = glyph["center"]
        result.append(f'<path d="{LETTERS[glyph["label"]]}" transform="translate({x} {y}) scale({glyph["letter_scale"]})" '
                      f'fill="none" stroke="{INK}" stroke-width="4.5" stroke-linecap="round" stroke-linejoin="round"/>')
    return "\n".join(result)


def document(body: str, width=128, height=128, title="GameCube button") -> str:
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">\n'
            f'<title>{title}</title>\n{body}\n</svg>\n')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--svg-only", action="store_true", help="Write source SVG/JSON without PNG exports")
    parser.add_argument("--resvg", default=shutil.which("resvg"), help="Path to the resvg renderer")
    args = parser.parse_args()
    if not args.svg_only and not args.resvg:
        parser.error("resvg was not found; install it or use --resvg PATH / --svg-only")
    OUT.mkdir(parents=True, exist_ok=True)
    exports = []
    for glyph in GLYPHS:
        svg = OUT / f'{glyph["name"]}.svg'
        svg.write_text(document(group(glyph), title=f'GameCube {glyph["name"]} button'), encoding="utf-8", newline="\n")
        exports.append(svg)
    # Preview text is itself simple SVG paths, so the complete sheet is font free.
    cells = []
    for index, glyph in enumerate(GLYPHS):
        x, y = 40 + (index % 4) * 196, 30 + (index // 4) * 184
        cells.append(f'<rect x="{x-12}" y="{y-12}" width="160" height="160" rx="8" fill="#111D32" stroke="#647086"/>')
        cells.append(f'<g transform="translate({x} {y})">{group(glyph)}</g>')
        # Second instance is shown at actual 32px UI size.
        cells.append(f'<g transform="translate({x+48} {y+131}) scale(.25)">{group(glyph)}</g>')
    preview = OUT / "preview.svg"
    preview.write_text(document('<rect width="856" height="420" fill="#070E20"/>\n'+"\n".join(cells),856,420,"GameCube glyphs: A B X Y / Z L R Start-Pause, with 32px samples"), encoding="utf-8", newline="\n")
    exports.append(preview)
    manifest = {
        "canvas": [128,128], "ui_draw_size": [32,32], "transparent": True,
        "do_not_crop": True, "font_independent": True,
        "reference": "https://www.nintendo.co.jp/ngc/control/control.jpg",
        "palette_note": "Flat UI interpretations of observed hardware colors, not official colorimetric specifications.",
        "palette": {**PALETTE,"outline":INK,"edge":EDGE},
        "glyphs": [{k:v for k,v in glyph.items() if k != "shape"} | {"svg": f'{glyph["name"]}.svg', "png": f'{glyph["name"]}.png'} for glyph in GLYPHS],
    }
    (OUT / "manifest.json").write_text(json.dumps(manifest,indent=2)+"\n",encoding="utf-8",newline="\n")
    if not args.svg_only:
        for svg in exports:
            subprocess.run([args.resvg,str(svg),str(svg.with_suffix(".png"))], check=True, capture_output=True)
    print(f"Wrote {len(GLYPHS)} glyphs and preview to {OUT}")


if __name__ == "__main__":
    main()
