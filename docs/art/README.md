# Melee UI artwork

The current menus retain Melee's original animated background, frame, footer,
menu cameras and native typefaces. Room/player fields and button actions are
live UI elements; concept screenshots are not loaded as complete screens.

## Current assets

The lobby controller and A/B/X/Y/Start prompts use cutouts from Nintendo's actual
indigo GameCube hardware photograph. The button lettering and colors come from
the same image. Source attribution, extraction masks, sizes and reproduction
steps are in the [controller notes](../../assets/ui/gamecube-original/README.md).
The earlier illustrated controller and authored SVG button glyphs are not the
selected runtime assets.

`tools/pack_netplay_art.py` packs the controller and matching PNG glyphs from
`assets/ui/gamecube-original`. It writes the renderer include and a source/hash
manifest under `build/art/netplay`. Text, selection, player panels and room
state are rendered separately. No generated background or chrome frame is
loaded over the original menu.

Menu text comes from the player's original NTSC-U 1.02 game data.
`tools/extract_netplay_fonts.py` extracts the SIS bitmap atlas and proportional
metrics from `main.dol`, and derives menu/panel glyphs from the original
`MnMaAll.usd` labels. `tools/menu_labels.py` prepares the added native labels,
including Widescreen. Generated disc-derived font/label data is excluded from
the source repository.

The 4:3 and 16:9 option graphics are flat, front-facing gray monitor symbols
with pale contours, explicitly following Melee's original `MnMaAll.usd`
Screen Display/controller equipment illustrations. Their screen openings are
exactly 4:3 and 16:9. `tools/build_native_monitor_art.py` produces editable SVGs,
PNGs and 128x96 native GX RGBA8 textures. The game attaches them to the original
animated option joints, so entrance, exit, selection and camera motion follow
the native menu. These replace the earlier generated monitor PNG prototypes.

Character select uses static portraits. The Mod Browser uses a selected pack's
existing, bounded, hash-verified PNG preview; it does not activate or download
the full package to display that image. Native live model previews have been
removed. Workshop's separate model/animation editor remains available.

Fierce Deity Link 1.0.2 is published through Workshop and installed locally.
Its completed Blender artwork provides four static character-select portraits,
four matching stock icons, and a clean Mod Browser preview. The renders use the
actual corrected skin and original Link artwork as the pose/lighting reference;
failed image-generation attempts and Workshop interface screenshots are not
packaged. The exact published package hash is
`048eaae4b7a9063d29ac06a4f2e42a123ddcf1cd0cfdf24cdbfd12b2521643b1`.
Mod artwork remains in that separate immutable package, outside the source
repository and base distribution.

## Reproduction and verification

Run the asset generators after preparing the player's disc data and the
separately attributed controller reference:

```powershell
python tools/menu_labels.py
python tools/extract_netplay_fonts.py
python tools/build_native_monitor_art.py
python tools/extract_original_gamecube_art.py
python tools/pack_netplay_art.py
```

The SVG raster steps require `resvg`; the Python image tools require Pillow.
Build and launch a matching runtime/renderer pair as described in
[building](../building.md). Visual capture and gameplay evidence belong to the
specific tested build; current results are tracked in
[netplay validation](../netplay-validation.md). Older art-only preview profiles
and captures do not establish the behavior of a later executable.

## Historical concepts

The original flat-monitor, room-browser and lobby concepts helped establish the
requested direction. Their explicit Melee screenshot references and prompts
are retained in [imagegen-prompts.json](imagegen-prompts.json) and
[netplay-assets-prompts.json](netplay-assets-prompts.json). Generated monitor
prototypes, the illustrated platinum controller, the authored glyph set and
the proposed foreground chrome remain historical iterations rather than the
current asset selection.
