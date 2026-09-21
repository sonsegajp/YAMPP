# Selective Akaneia content importer (development)

This tool builds a local test image from the player's verified Melee USA 1.02 disc and a verified official Akaneia 1.0.1 release. It imports seven added fighters, 17 versus stages, their seven target-test stages, and 41 additional music tracks. Stage playlists and fighter victory themes retain their references. Volleyball is a separate mode and is excluded.

The import starts from clean Melee data. It never copies Akaneia's main menus, optional codes, modes, rule changes, stock fighter/stage replacements, or menu playlist. The m-ex core supplies the expanded content tables. YAMPP's host menu/input/music hooks still apply. Main-menu and SIS asset hashes, base fighter/stage/music records, and every added music file are checked. A generated `content-audit.json` records the exact local import.

Prerequisites: .NET 8, official MexManager source revision `aecc921dbd1e13c2c9381f8fae18400cba32a8e0` under `upstream/MexManager`, the pinned release's m-ex core metadata, and locally extracted official release files. The executable resolves ImageSharp 3.1.11. Upstream source, ISO files, generated project data and music stay in ignored directories. Mod content, images, generated project data and music are never committed, mirrored on the relay, or bundled into the base distribution. Only the importer tools are bundled.

```
python scripts/patch_mex_disc.py
dotnet build tools/modkit/mex_bridge/MexContentBridge.csproj -c Release -o build/modkit/mex-content-bridge
dotnet build/modkit/mex-content-bridge/MexContentBridge.dll compose SOURCE_DIRECTORY CORE_JSON YOUR_BASE_ISO build/akaneia-content/NEW_OUTPUT
python scripts/check_akaneia_content.py build/akaneia-content/NEW_OUTPUT
```

Use a new output directory for each composition. `SOURCE_DIRECTORY` must be a dedicated extracted release workspace; the upstream importer generates its project metadata there. `inspect` prepares this workspace without composing a new image.

The Windows native runtime now activates verified selective content through Mod Manager and the room-join confirmation flow, preserving its window. Official release acquisition, isolated import and runtime activation remain separate validation steps. Impaired-connection rollback, results and rematches have been checked; see [Akaneia validation](../../../docs/akaneia-testing.md) and [Online validation](../../../docs/netplay-validation.md). This does not imply compatibility with arbitrary m-ex projects.

The referenced MexManager and m-ex checkouts are not included in this source repository. Windows preview releases include the local importer. Neither pinned repository supplies a top-level license; this project does not infer a license grant from that absence. Available dependency notices and exact source references are retained in CREDITS.md and licenses/third-party/content-importer.

Supplemental stage dependencies are collected recursively from stage archives, retained at their original runtime filenames, and recorded with hashes. The independent content audit also reads the exported ISO and verifies all 51 included stage files against the official source.

The reviewed source patch opens the base ISO for shared read-only access, so preparing content while YAMPP is running does not collide with the game's disc handle. It validates disc ranges and filenames before extraction. Packaged preparation requires 6 GB free for temporary files, serializes concurrent attempts and removes each attempt's temporary disc/source after completion or failure, retaining its log. The verified official archive remains available for retries.
