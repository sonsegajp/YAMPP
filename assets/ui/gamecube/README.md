# GameCube button glyphs

Flat, front-facing glyphs for the Melee-style netplay UI. Shapes and color families
were checked against Nintendo's [original GameCube controller photograph](https://www.nintendo.co.jp/ngc/control/control.jpg),
on the [original hardware controller page](https://www.nintendo.co.jp/ngc/control/index.html).
Button names were cross-checked against Nintendo's [GameCube controller manual](https://csassets.nintendo.com/noaext/image/private/t_KA_PDF/gcncontrol?_a=DATAg1AAZAA0).
This reference is the original wired GameCube controller, not the later Switch 2 version.

The silhouette paths are original authored UI interpretations. Palette values are
chosen for legibility and matching the observed hardware color families; they are
not official measured Nintendo color specifications. Outlines are flat dark ink
`#172132` with a thin ivory `#F0EAD6` edge. There are no gradients or perspective.

| Files (SVG and PNG) | Geometry | Fill |
| --- | --- | --- |
| `a` | Large circle, radius 48 | Green `#32B895` |
| `b` | Small circle, radius 34; 71% of A diameter | Red `#DF4049` |
| `x` | Tall gray kidney/oval, inner edge facing left toward A; top leans left | Gray `#BBC0CA` |
| `y` | Wide gray kidney/oval above A; outer edge rises toward the right | Gray `#BBC0CA` |
| `z` | Narrow shoulder button | Purple `#8175C8` |
| `l`, `r` | Broad trigger shapes with curved tops and tapered bases; mirrored outlines | Gray `#BBC0CA` |
| `start-pause` | Small blank circle, radius 24 | Gray `#BBC0CA` |

Each individual glyph has a transparent **128 × 128** canvas. Draw every glyph at
the same **32 × 32 logical pixels** in the UI; preserve its canvas and aspect ratio.
Do not crop to the opaque bounds or scale B to A's diameter. X and Y retain their
different orientations while the letter remains upright. Letters are SVG paths,
so exported artwork has no runtime font dependency. Start/Pause is intentionally
blank like the physical button; pair it with an adjacent `Start` or `Start/Pause`
caption when used in a prompt.

`manifest.json` records the palette, filenames, shared canvas, and placement
conventions. `preview.svg` / `preview.png` show A, B, X, Y on the first row and Z,
L, R, Start/Pause on the second; each includes a 32-pixel rendering below it.

Regenerate all SVG and transparent PNG files from the authored geometry:

```powershell
python tools/build_gamecube_glyphs.py
```

The script uses `resvg` from PATH for deterministic raster export. Pass
`--resvg C:\path\to\resvg.exe` to choose a renderer, or `--svg-only` to write only
the SVG sources and manifest. All generated files stay in this directory.

When reviewing runtime screenshots, check that the green A is visibly larger than
the red B, X stays tall, Y stays wide, letters remain upright and legible, and the
32-pixel canvas leaves a consistent gap before each action caption. L/R are gray,
Z is purple, and no face button gets a generic circle substitute.
