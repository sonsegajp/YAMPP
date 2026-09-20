# Melee Workshop desktop editor

Open `Run-Workshop.cmd` in the project root. The packaged application is
`build/workshop/MeleeWorkshop-win32-x64/Melee Workshop.exe`.
Keep a development build inside the YAMPP checkout: it uses the extracted assets
and HSD bridge. A distribution supplies its private Python runtime at
`tools/python/python.exe`; Workshop prefers it over an installed Python 3.10 or
`python` on PATH. `MELEE_WORKSHOP_PYTHON` overrides that selection.

The editor runs in Electron with native File/Edit/View/Tools menus, native
Open/Save dialogs, and an isolated Python asset worker over private pipes.
It does not start an HTTP server or open a browser. Closing the app closes
its worker. Opening it again focuses the existing window.

## Use

- Select a fighter to inspect its model, costumes, actions, damage events,
  common attributes, and underlying DAT data. The timeline previews animation
  and hitboxes; conditional script behavior is shown as data rather than simulated.
- Use **Submodels** or the **Model preview** selector to inspect alternate forms separately. The main view respects costume visibility tables and excludes low-detail duplicates. Samus Bomb follows the script frame by frame; Kirby held Stone previews one form, with all five available individually. Export saves only the visible form for a clean Blender import. Callback-driven transitions are identified in the UI.
- Clone a fighter to create a separate package under `user/mods`. Stock data
  stays read-only. No example fighter or costume packages are bundled with the
  source repository or base distribution.
- Use **File > Import Blender stage** (`Ctrl+O`) to choose a `.glb` or `.gltf`.
  Companion buffers and textures are collected from the glTF scene folder,
  including subdirectories. Stage imports currently require static geometry;
  bake unsupported armatures/extensions before importing.
- Set collision, spawn points, and boundaries, then compile the stage package.
- Use **File > Export GLB** (`Ctrl+E`) to save through the native file dialog.
  Optional character animation export can take several minutes.
- **Test Game** launches with enabled local fighter/stage mods for that process
  only. Packaged builds reuse the native Setup extraction and remembered disc
  (including RVZ/WIA); normal-launch preferences remain unchanged.
- Use **Tools > Save Blender addon** to save the addon for installation in Blender.

Workshop's model and animation preview remains available. In-game character
select uses static portraits; the experimental native 3D previews and Mod
Browser model inspector were removed. The in-game Mod Browser fetches only a selected pack's bounded, hash-verified PNG
for its static preview area. Missing or invalid art shows **Preview unavailable**;
thumbnail browsing does not download or activate the full package.

Character and stage assets retain the existing pipeline. Native in-game custom
roster and stage integration is still being validated separately; desktop checks
establish editor import, conversion, and rendering, not complete in-game support.
Brawl disc extraction is not implemented in this editor yet.

## Additive costumes and publishing

**New costume pack** copies selected stock costumes into a separate package.
The **Costumes** library edits visual textures while preserving the fighter's
original model/animation compatibility and shared gameplay data. Packs add
colors after the originals rather than replacing their slots. Each uploaded
pack supports 1-16 colors; a fighter supports 64 combined stock and added colors.

Use **Publish a pack**, then **Prepare upload** to review its exact files and
metadata before **Upload reviewed pack** sends them. Publishing requires a
configured private publisher credential. Downloads verify the complete immutable
ZIP hash, declared payloads and stock-compatible DAT roots. An unrelated local
package with the same ID is preserved and reported as a conflict.

**Online > Mod Browser** installs and enables/disables published costume packs.
Rooms require exact package hashes and ask before downloading or activating a
different set. Fighter replacements, Lua and custom stages are outside the
supported Online package set. See [Workshop community](../../docs/workshop-community.md)
for the client commands and [modding](../../docs/modding.md) for package authoring.

Fierce Deity Link 1.0.1 is published separately with teal, red, green and violet
colors. Its fourteen neutral/animated eye images and low-detail eye regions are
corrected; the original geometry, skeleton, UVs and animation tracks remain.
The existing package preview is retained. New generated character-select
portraits and four matching stock icons remain pending and are not part of
this revision. Optional per-color portrait/icon fields and verified native
texture sidecars are supported for future art packages.

Public GitHub/source-repository and release publication remain on hold. This is
separate from the live Workshop package service. Original game data, installed
mods, generated mod artwork and publisher credentials stay outside the source
repository and base distribution.

## Build and check

Source-build prerequisites: Node/npm, Python with NumPy and Pillow, .NET 8,
the HSD bridge, and legally extracted Melee USA 1.02 assets in
`data/GALE01/files`. Distribution users use the bundled interpreter; its isolated
search path excludes system/user packages and ambient `PYTHONHOME`/`PYTHONPATH`.

```powershell
npm.cmd --prefix tools/modkit/desktop ci
npm.cmd --prefix tools/modkit/desktop run check
npm.cmd --prefix tools/modkit/desktop run package
.\Run-Workshop.cmd
```

`npm ... start` runs the development Electron app. The legacy `server.py` entry
point now opens the packaged desktop application instead of serving a webpage.

Desktop checks launch the actual Electron executable, inspect presented frames,
exercise character data and animation, export GLB, import GLB and nested glTF
companions, compile a stage, and verify canceled imports and isolated IPC.
They store screenshots and a JSON report in `build/modkit/electron-check`, with
a separate test mod directory. Native-dialog return values are deterministic in
the check; interactive builds always use the real Windows dialogs.

Electron is pinned by `desktop/package-lock.json`. Electron license files ship
with the executable. The HSD converter uses upstream HSDLib (MIT); Three.js
license text is retained under `web/vendor`. No original game data is bundled
into the desktop application.

For an offline portable runtime, run `python scripts/stage_workshop_python.py`
with an installed 64-bit Python 3.10 that already has NumPy and Pillow. The
script stages only its standard library, runtime DLLs and these two packages
under `build/distribution-python`, retaining their complete license notices and
writing a file-hash manifest. The release builder places that tree at
`tools/python`. An existing staging destination is refused to avoid stale files.

The staged Python 3.10.11 / NumPy 2.2.6 / Pillow 12.2.0 runtime passed an actual
private worker `ping` and `read /api/catalog`: 26 original fighters and 120 stock
costume DATs, no worker errors, with invalid ambient Python paths ignored.
Evidence is in `build/modkit/portable-python-check/report.json`; the file count
is distinct from costume-slot count because some stock colors share a DAT.

The PNG-only preview client and native hidden worker passed against the existing
published preview. Final screenshot validation of the updated renderer is tracked
separately in [netplay validation](../../docs/netplay-validation.md).
