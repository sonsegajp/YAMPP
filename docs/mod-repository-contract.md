# Melee costume repository contract

The community repository extends rollback protocol 2 with `mods-v1`; ordinary
unmodded rooms keep the existing behavior. Only additive visual costume packs
are accepted. They never replace a stock costume or contain gameplay scripts.

## HTTP API

The public base is `https://mmodx.fun/melee/api`. Local tests use
`http://127.0.0.1:<port>/api`. The same server handles both path prefixes.

- `GET /mods` returns `{"schema":1,"mods":[<metadata>,...]}`.
- `GET /mods/<sha256>` returns one metadata object.
- `GET /mods/<sha256>/package.zip` returns the original ZIP bytes with an ETag
  equal to the quoted lowercase SHA-256, and `application/zip` content type.
- `GET /mods/<sha256>/preview.png` returns validated PNG artwork when present,
  with a content-hash ETag. Metadata includes `preview_path`, `preview_sha256`,
  and `preview_size`; packages without artwork return 404.
- `POST /mods` accepts raw `application/zip` bytes and
  `Authorization: Bearer <publisher-token>`. The server token comes from
  `MELEE_MOD_UPLOAD_TOKEN`. The response is metadata, status 201 for a new
  content hash and 200 for a package already present. Uploads remain disabled
  when the server token is unset. Tokens never appear in metadata or logs.
- Errors have `{"error":"human readable reason"}` with an appropriate HTTP
  status. There are no redirects to third-party package hosts.

Metadata:

```json
{
  "sha256":"<64 lowercase hex characters>",
  "id":"fierce-deity-link",
  "name":"Fierce Deity Link",
  "version":"1.0.0",
  "kind":"costume",
  "base":"stock-Lk",
  "additive":true,
  "size":123456,
  "costumes":[{"id":"fierce-deity","name":"Fierce Deity","file":"files/PlLkFD.dat"}],
  "download_path":"/melee/api/mods/<sha256>/package.zip"
}
```

Clients must derive the download origin from the configured trusted repository,
verify the full downloaded archive SHA-256, and validate the ZIP and manifest
before writing any files. Installation is into a new per-package directory,
never stock assets. Publishing a changed archive produces a different immutable
hash. Installing a particular room uses its exact hash, not a same-name update.

## Package format

ZIP members are relative POSIX paths. The root contains `manifest.xml` in the
existing typed XML format (`melee-package`, schema 1). Its decoded object is:

```json
{
  "schema":1,
  "id":"fierce-deity-link",
  "name":"Fierce Deity Link",
  "version":"1.0.0",
  "kind":"costume",
  "base":"stock-Lk",
  "additive":true,
  "costumes":[
    {"id":"fierce-deity","name":"Fierce Deity","file":"files/PlLkFD.dat"}
  ]
}
```

`schema`, `enabled`, `author`, and `description` are optional metadata keys.
`schema`, if supplied, is 1. `enabled` is a boolean installation preference.
Each costume may additionally declare relative PNG `portrait` and `stockIcon` paths and validated native texture sidecars described below.
An optional root `preview.png` is permitted. Only the declared costume/art payloads and supported metadata are accepted. There are 1-16 costumes per pack; the runtime caps the combined
stock-plus-added count at 64 per fighter. Internal DAT public model/skeleton
symbols stay compatible with the stock fighter; filenames may be distinct.

Allowed bases are original stock fighter IDs (`stock-Lk`, `stock-Fx`, etc.).
No DOL/REL, fighter common DAT, animation bank, stage, executable, script, Lua,
JSON patch, archive symlink, absolute path, traversal, duplicate member name,
or case-insensitive filename collision is accepted. Limits are 64 MiB for a
ZIP, 128 MiB expanded, 64 members, 32 MiB per DAT, and 4 MiB per PNG.

For reproducible hashes, packers sort member paths, use a fixed ZIP timestamp
`1980-01-01 00:00:00`, fixed compression settings and permissions, and serialize
the typed XML consistently. The server hashes the uploaded bytes unchanged.

## Room negotiation

The welcome includes `features:["compat-v1","mods-v1"]` and
`mod_api:"https://mmodx.fun/melee/api"` on the public server.

- A client must advertise `features:["mods-v1"]` in its hello to host or join
  modded rooms. Features cannot be renegotiated while in a room.
- Host `create` adds `required_mods:["<sha256>",...]` (maximum 8 distinct
  published hashes) and `installed_mods:["<sha256>",...]` for verification.
- A room summary and detail include `required_mods:[<metadata>,...]`. Room
  metadata omits the full `costumes`, `description`, and `author` fields; fetch
  those from the HTTP API by hash. Room-list replies fit 60 KiB and at most 32
  rooms, with `more:true` if additional rooms did not fit.
- Guest `join` adds `installed_mods:["<sha256>",...]`. This is the exact active
  room costume set, not every disabled package stored on disk.
- If the active hashes do not match the required set, the server replies
  `{"op":"mods_required","room":123,"mods":[<metadata>,...]}` and does not
  change room membership. The client displays the requirement list and waits
  for explicit download/activation consent before fetching anything.
- After successful download, validation, installation and activation, the client
  retries `join` with the exact hash set. Only an ordinary `room` acknowledgement
  transitions the menu into the lobby. Cancel leaves the player in the browser.
- Invalid hashes, unknown packages, or non-costume packages are rejected before
  room creation or membership changes. Required hashes are fixed for the room's
  lifetime and included in session start, so both peers use the same set.
  A successful join records that client's acknowledged set; Ready and Start
  recheck it against the room requirements.

The server can check the declared hashes; the client is responsible for hashing
installed bytes and ensuring those are the packages actually loaded. Protocol 2
rollback remains unchanged. The server never interprets or executes mod assets.

Current clients also negotiate the [exact build and base-data fingerprint](netplay-compatibility.md).
That check runs before the costume requirement prompt and does not silently
downgrade when joining a legacy-only room.

## Static artwork

The game uses static previews. The earlier native 3D model inspector has been removed. Read-only package inspection remains a tooling operation and never enables or replaces an installed package.

Each declared costume may include optional `portrait` and `stockIcon` PNG paths.
Both are visual payloads covered by the immutable package hash, safe-path checks,
PNG dimension limits and exact declared-file allowlist. They do not change gameplay.

Optional `portraitTexture` and `stockIconTexture` paths must end in `.gxt`.
The exact format is 8-byte `YAMPPTEX`, four big-endian u32 values (width,
height, GX format, payload length), then exactly that many GX bytes. Only
CMPR format14 is accepted: portrait136x188 with13056 payload bytes, or
stock icon24x24 with288 payload bytes. These declared visual sidecars are
covered by the same package hash and strict file allowlist.


## Room-only versions

The native consent flow runs `community.py room-prepare --hashes <sorted CSV>`
once for the complete required set. A matching verified local package can be
used directly. Missing or conflicting versions are verified into an ignored,
per-installation cache under `build/modkit/cache/community/rooms`; current local
packages, author edits, enabled preferences and stock files are never replaced.
The ordinary Mod Browser install operation still rejects same-ID collisions.

Cache reuse verifies the full immutable ZIP and every extracted file, including
manifest order and optional artwork. Only after the entire set verifies does an
atomic costume registry replacement add the needed cached rows, marked disabled
for offline play. Native exact-hash selection occurs before duplicate-ID checks;
a room cannot activate two versions of one ID. Leaving or canceling restores the
existing enabled local rows. A canceled download may retain inert cache bytes,
but cannot enter the lobby or enable them offline.

The cache is bounded to 16 packages and 2 GiB, with safe pruning restricted to
unselected direct cache children. Linked/reparse paths are rejected before
removal. The current requested hashes are never evicted. Room packages are still
limited to8 and 64 combined slots per fighter. Costume-only community operations
emit `costumes.tsv` without invoking the legacy fighter registry or .NET bridge.
