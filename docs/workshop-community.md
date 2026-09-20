# Workshop costume packs and Mod Browser

Open `Run-Workshop.cmd`. **New costume pack** copies selected original colors
into a separate additive visual package. The original stock costumes and shared
fighter/gameplay data remain unchanged. The new **Costumes** library displays
the package's actual model, animations and editable textures. Gameplay editing,
Lua, and shared roster icon replacement are unavailable for costume packs.
The preview infers the source color from its preserved DAT root, including
costumes that use different visibility tables.

**Online > Mod Browser** in the game lists published costume packs with their fighter, color names,
version, author and description. Downloads run through the private worker with
progress. Installation verifies the selected complete ZIP SHA-256, then reuses
the production repository's strict archive/manifest/DAT validator. Only a new
package directory is installed; an unrelated package with the same ID is
preserved and reported as a conflict. An unchanged installed hash is idempotent.
Use Enable/Disable to choose locally active packs. The game applies costume changes in menus before entering a room.

To publish, choose **Publish a pack**, select a local additive costume pack,
enter metadata, and click **Prepare upload**. The review lists the exact
additional colors and included files. Only **Upload reviewed pack** sends it to
the repository. The prepared ZIP has sorted paths, fixed 1980 timestamps and
fixed compression settings. Installation preferences and private hash records
are excluded from the published manifest. If the author's local payloads still
match the reviewed upload, that source is recorded as a verified installed copy.

Repository configuration lives in the private, ignored `user/community.xml`:

```xml
<community>
  <repository>https://mmodx.fun/melee/api/mods</repository>
</community>
```

The publisher credential is read from `MELEE_MOD_UPLOAD_TOKEN`, or the optional
private `publisher-token` element in that file. It is never returned through IPC,
passed in frontend command arguments, included in packages, or written to logs.
The UI receives only a configured/not-configured flag. HTTPS is mandatory except
for loopback integration tests. Download addresses are derived from this
configured origin; redirects and metadata-supplied external hosts are rejected.

Native Mod Browser commands:

```powershell
python tools/modkit/community.py list --output build/mod-list.json
python tools/modkit/community.py active --output build/mod-active.json
python tools/modkit/community.py install --sha256 HASH --output build/mod-result.json
python tools/modkit/community.py toggle --sha256 HASH --enabled 1 --output build/mod-result.json
```

Outputs are atomic JSON files; failures return `{"error":"..."}` and exit 1.
`active` verifies every enabled costume pack and fails if one is unpublished or
modified. `install.json` binds the original package hash to every current payload
hash and the decoded public manifest hash, excluding only local enabled state.
The installer and enable command enforce 64 combined stock/additional costumes
per fighter; hosting supports at most eight verified active packages.

Validation:

```powershell
python tools/modkit/test_community.py
python scripts/check_workshop_community.py
```

The contract check clones four actual stock Link DATs and verifies deterministic
packing, upload/list/download/install, same-ID collision preservation, source
file integrity, enable/disable and modified-file rejection. It hashes stock
assets before and after. The Electron check drives the actual UI against the
production server in an isolated local repository, with private test credentials
and test-only mod folders. Screenshots and JSON reports are retained under
`build/modkit/electron-community-check/<timestamp>/`. These checks do not publish
test packs to the public repository.


The game displays the selected package's verified static PNG thumbnail. This fetches only bounded metadata and PNG bytes from the trusted repository, validates the preview hash and PNG structure, and never downloads or enables the full package. The experimental native 3D inspector was removed. The internal read-only `community.py inspect` cache helper remains available for tooling; it downloads and validates exact immutable package files without installing or enabling them.

The costume registry is independent of the legacy `modsEnabled` switch. `MELEE_COSTUMES\t3` has sixteen tab-separated fields: `C, packid, packageSha256-or-dash, enabled, externalKind, internalKind, prefix, slotId, name, absoluteDatPath, baseColor, datSha256, portraitTexturePath, portraitTextureSha256, stockIconTexturePath, stockIconTextureSha256`. Missing art uses `-` for both path and hash. Native loading still accepts twelve-field version 2. Disabled packages remain available for exact room-hash activation. Published/downloaded copies record their verified hash; edited offline packages use `-` and cannot host a verified Online room.

Fierce Deity Link 1.0.1 adds four colors (teal, red, green, violet), SHA-256 `2200a1aa2b36df6f6aefe22b665bbb3c58771f2c32eaa1c233636e71bcce9393`. This revision corrects all fourteen neutral/animated eye images while retaining the original model, skeleton, UVs, animation tracks and existing preview. The prior immutable 1.0.0 remains available. New generated portraits and matching stock icons are pending; this update does not include substitute renders. Mod files/artwork remain outside the source repository.


## Joining rooms with another costume version

A room requires an exact immutable package hash. If those hashes differ from
your active set, joining pauses outside the lobby and displays the required
packs. Confirming prepares all required versions and activates precisely that
set before retrying the same room. Canceling returns to the browser.

A room download uses a separate ignored cache, even when an installed package
already has the same ID with a different version or unpublished author edits.
Your installed files, enabled preferences and stock files remain untouched.
Cached versions are disabled for offline use; leaving or canceling restores your
normal enabled costume set. The ordinary Mod Browser installation still reports
same-ID conflicts instead of replacing an existing package.

The helper `community.py room-prepare --hashes <sorted comma-separated SHA-256s>`
checks the entire room set before atomically updating its costume registry.
Cached ZIPs, manifests and every extracted payload are revalidated on reuse.
A room supports up to 8 packages and 64 combined slots per fighter; activating
both versions of the same package ID in one room is rejected. The room cache is
limited to 16 packages and 2 GiB; only old, unselected cache entries can be pruned,
and linked/reparse paths are rejected. Canceling can retain inert cached bytes,
but cannot install, enable or join with them.

`python tools/modkit/test_room_cache.py` covers old/new version swaps, unchanged
author files, modified-cache rejection, atomic failures and bounded pruning.
When the focused native worker test is built, it also runs the real hidden
consent worker and checks exact-room handoff and cancellation. The production
costume-loader check covers selecting one same-ID version and restoring the
offline version. Costume-only community operations do not generate the legacy
fighter registry or require the .NET model bridge.
