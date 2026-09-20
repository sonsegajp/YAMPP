# Making mods with Melee Workshop

Workshop runs beside YAMPP and uses the same locally extracted USA 1.02 assets. Open **Melee Workshop.cmd** in a distribution or **Run-Workshop.cmd** in a checkout. The desktop needs Python with NumPy/Pillow and the .NET 8 HSD bridge unless those runtimes are included in your prepared package. See [building](building.md) and the [Workshop README](../tools/modkit/README.md).

## Choose the package type

| Type | What it changes | Where it works |
| --- | --- | --- |
| Additive costume | A fighter's visual costume, optional portrait and stock icon | Local play; current Online/Workshop repository |
| Fighter clone | A separate local fighter package based on an original fighter, attributes, move data and optional Lua | Local development; each character needs in-game validation |
| Stage | Imported static geometry, collision platforms, spawns, camera and blast bounds | Local development; editor compilation is not proof of complete gameplay support |
| Custom music | Locally decoded replacement for a music track | Local play; see [music](custom-music.md) |

Keep each package in its own `user/mods/<unique-id>` folder. Do not edit files in `data/GALE01`. Never copy mods or extracted assets into the Git repository. A new package ID identifies a separate mod; a new version/hash identifies a revision of that mod.

## Add a texture skin without replacing stock colors

1. Select an original fighter and choose **New costume pack**. Name the pack and choose its source stock colors. Workshop copies these into a new package; the originals remain unchanged.
2. Select the new entry in the **Costumes** library, then select a costume and a texture. Export its PNG, repaint it, and import it at the exact original dimensions. UV coordinates are retained. Keep faces, hands and clothing inside the original UV islands.
3. Inspect every relevant texture and animation. Eye/blink and mouth animations can use separate image frames and palettes; repainting a neutral face alone leaves the other expressions unchanged. Low-detail models can have their own textures too.
4. Test all added colors in character select and a match, including attacks, damage, blinking, transformations where applicable, stocks and results. Check another player using the original color at the same time.
5. Enable the pack locally. Additional slots follow all stock colors. Multiple enabled packs are sorted by immutable package hash then package ID, preserving the declared order inside each pack. Do not rely on a fixed numeric slot across different active pack sets.

Each costume has a unique ID within its package and a DAT file with the original fighter's compatible public roots. Do not replace the skeleton or shared animation/gameplay files for a costume pack. The combined limit is 64 stock and added colors per fighter, with 1-16 costumes per uploaded pack. Dependent stock-only tables such as Kirby copy hats retain their original bounds.

### Portraits and stock icons

The decoded manifest's costume entry may declare `portrait` and `stockIcon` PNG paths. Native menu/HUD art additionally uses `portraitTexture` and `stockIconTexture` sidecars. A sidecar contains `YAMPPTEX`, four big-endian 32-bit integers (width, height, format, payload length), then GX texture bytes. Only CMPR format 14 is accepted: 136x188 / 13,056 bytes for a portrait, 24x24 / 288 bytes for a stock icon. The 24-byte header is additional.

Art files must be inside the package and included in its reviewed upload. Package and individual payload hashes cover these files. Keep transparent backgrounds, frame the portrait to fit its original panel, and make the 24x24 face silhouette readable at native resolution. Stock and other players' artwork must remain independent. Missing optional art falls back to the original base color.

The package manifest is typed XML, written by `tools/modkit/xml_manifest.py`; do not paste JSON directly into `manifest.xml`. [The repository contract](mod-repository-contract.md) shows the decoded schema and strict accepted file types.

Prepare the final-sized transparent PNG canvases in the package's `art` folder, then encode them with the bridge:

```powershell
python tools/modkit/native_texture.py user/mods/my-pack/art/portrait.png user/mods/my-pack/art/portrait.gxt
python tools/modkit/native_texture.py user/mods/my-pack/art/stock.png user/mods/my-pack/art/stock.gxt
```

Assign those relative paths to the intended costume using the typed manifest helper. Run this Python snippet from the project root after substituting your package and costume ID; it edits only that local package:

```python
import sys
from pathlib import Path
sys.path.insert(0, "tools/modkit")
from xml_manifest import read_manifest, write_manifest
folder = Path("user/mods/my-pack")
manifest = read_manifest(folder)
costume = next(c for c in manifest["costumes"] if c["id"] == "color-1")
costume.update(portrait="art/portrait.png", stockIcon="art/stock.png",
               portraitTexture="art/portrait.gxt", stockIconTexture="art/stock.gxt")
write_manifest(folder, manifest)
```

Use distinct art paths for each color. Regenerate the local registry by reopening Workshop, then test the package. Prepare a new reviewed version before publishing any changes to an already published pack. In a distribution, `tools/python/python.exe` runs these same Python commands.


## Create a local fighter

Select an original fighter and clone it. Workshop creates a separate folder with copied fighter/costume/animation data and a manifest. Edit exposed attributes and move data on the clone, save, and preview the affected animation and script events. Conditional events in the editor are descriptions rather than a full game simulation.

Use **Submodels** to inspect alternate forms. **Export GLB** exports the selected visible model for external editing; optional full animation export can take several minutes. Native exports retain the original split normals, UV seams, skin weights and bone hierarchy, and convert GameCube triangle front faces to glTF's counterclockwise convention. Preserve those authored normals when posing a model in Blender; blanket smoothing can erase intentional hard edges. Export support does not imply arbitrary replacement rigs can be imported as a working fighter. Keep the base fighter's engine callbacks, bone conventions and animation expectations unless implementing and testing the required runtime changes.

For additional behavior, use the package's Lua editor and [Lua API](lua-api.md). Scripts cannot access the filesystem, network or native modules. Test interruption, hitboxes, landing, damage, stocks and results in game. Enable legacy gameplay mods explicitly in the development configuration; additive costumes use their own registry. Local fighter/stage integration remains experimental and is not part of Online compatibility.

## Build a local stage

1. Create static geometry in Blender and export `.glb` or `.gltf`. Bake unsupported modifiers/armatures/extensions. Include companion buffers/textures alongside a `.gltf` and preserve relative subfolders.
2. Use **File > Import Blender stage**. Inspect the imported geometry and materials.
3. Define collision platforms as left-to-right endpoint pairs, set 1-4 player spawn points, and set ordered camera and blast bounds. Add at least one platform; coordinates must stay within the documented editor range.
4. Compile the package. The bridge builds a stage DAT using the local template and saves a GLB preview. Keep the generated files under the package directory.
5. Test all spawn points, floor/edge collisions, camera movement, blast zones, respawns and match exit in game. A successful conversion or editor preview alone does not establish a playable stage.

## Publish and join

**Publish a pack** currently accepts additive costumes only. Enter metadata, choose **Prepare upload**, review every included costume/file, then choose **Upload reviewed pack**. Upload requires the configured publisher credential; the current service has no public account-registration or credential-issuance UI. Anyone can create packages locally; hosting operators decide who receives publishing access.

Published ZIPs are immutable. Changing textures or art creates a new hash/version; do not overwrite a published payload under its old hash. Use **Online > Mod Browser** to view and install a pack. Joining a room with a different exact costume set shows a prompt before downloading/activating it. Cancel leaves the browser unchanged. The server checks the required set again before Ready/Start.

See [Workshop community workflow](workshop-community.md) for configuration and command-line checks. Credentials stay in ignored local configuration or environment variables, never source, screenshots, logs or uploaded packages.

## Akaneia content mods

The managed development build imports only the pinned official Akaneia fighters, stages and music, with consent before GitHub downloads and a toggle in Mod Manager. See [selective Akaneia testing](akaneia-testing.md) for the supported scope, launcher, room-join flow and current limits. This is separate from the local fighter/stage authoring paths above.
