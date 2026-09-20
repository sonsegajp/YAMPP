# Original GameCube controller and matching buttons

These are cutouts from Nintendo's actual original indigo wired-controller
photograph, not generated illustrations or redrawn button lettering.

Source: [Nintendo high-resolution GameCube hardware photograph](https://www.nintendo.co.jp/ngc/thisis/grap/gamecube.jpg),
published on Nintendo's [original hardware image page](https://www.nintendo.co.jp/ngc/thisis/grap/index.html).
The full source is 880 × 1170; the controller occupies its lower portion.
`manifest.json` records its SHA-256, local source path, exact crop coordinates,
and output files.

`controller.png` is the transparent 429 × 340 controller cutout, including its
short visible cord loop. `a.png`, `b.png`, `x.png`, `y.png`, and `start-pause.png`
contain the actual buttons and lettering from that same photograph. No button
was repainted, relabeled, or recolored. L/R are partly hidden behind the shell
and Z is edge-on, so complete LRZ glyphs are not claimed or fabricated here.

All five button files use a common 128 × 128 transparent canvas and should be
drawn at **32 × 32 logical pixels**, without cropping their alpha bounds. This
preserves the source's large green A, smaller red B, tall gray X, wide gray Y,
and small gray Start/Pause. Their source pixels are placed on 64 × 64 canvases
and duplicated exactly 2× using nearest-neighbor; relative physical size is
preserved. `preview.png` includes these small UI-size examples.

Reproduce the extraction:

```powershell
python tools/extract_original_gamecube_art.py
```

The script requires Pillow, `resvg` on PATH, and the downloaded source at
`build/ui-art-references/nintendo-gamecube-original-hires.jpg`. It preserves
source RGB pixels, adding only authored alpha masks, crop boundaries, and
nearest-neighbor duplication. The controller mask also removes neutral source
paper pixels connected to the exterior; enclosed white lettering survives.
The original-coordinate SVG masks and their rasterized alpha references are
in `masks/` for review. They are extraction masks, not replacement artwork.

The earlier authored SVG glyphs remain separately in `../gamecube/`; this
directory is the source for the user's corrected real-controller direction.
