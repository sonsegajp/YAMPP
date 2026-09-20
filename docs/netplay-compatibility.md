# Netplay compatibility verification

Rollback protocol 2 advertises the additional `compat-v1` feature. A current
client hashes its actual native executable, the loaded DOL, configured FST,
all extracted base-game files in canonical relative-path order, and the disc
image read by the native DVD backend. The DOL must match the project's pinned
Melee revision. Renderer DLL bytes are excluded.

Verification runs in a hidden helper while the game remains responsive. Before
hashing, the Windows host opens every input with read sharing only; those handles
deny writes and replacement until shutdown. Linux holds shared advisory locks and
rechecks file/path identity, size and nanosecond timestamps before room activation
and session gates. Advisory locks do not prevent writes by an unrelated process
that ignores them. The verified result is cached only
in that process; modification times or an old extraction manifest never stand
in for content verification. Package payloads have a separate immutable hash
and activation gate, described in [the repository contract](mod-repository-contract.md).

The hello adds:

```json
{
  "features": ["compat-v1", "mods-v1"],
  "compatibility": {
    "schema": 1,
    "fingerprint": "<combined SHA-256>",
    "runtime": "<runtime compatibility identity>",
    "game": "<canonical base-input SHA-256>"
  }
}
```

The combined digest hashes the bytes `MeleePC-compatibility-v1\0` followed by
the decoded runtime and game digests. The helper's source defines the versioned,
length-prefixed base-input encoding. Install directories and enumeration order
do not affect identity. Runtime bytes, base-input bytes and camera aspect do.

The helper preserves the exact executable SHA-256 as `runtimeFileSha256` (and
its legacy `runtime` field). It separately derives `runtimeIdentity4x3` and
`runtimeIdentity16x9`: SHA-256 of `YAMPP-runtime-camera-aspect-v1\0`, the decoded
file SHA-256, and one byte (`0` or `1`). `runtimeIdentitySchema` is 1 and must
match the native `NM_RUNTIME_IDENTITY_SCHEMA`. Each corresponding `aspect4x3`
or `aspect16x9` fingerprint uses the original combined-digest formula above.
Only the chosen compatibility identity is advertised as the wire `runtime`.
The relay's existing consistency check remains unchanged; no renderer bytes or
persistent file-hash cache replace verification of the actual executable.

An Online connection request atomically acquires the current aspect before
queued verification or hello. Both F1 and native Options reject aspect edits
until disconnect and explain: "Disconnect from Online to change aspect ratio."
Leaving a room keeps the lock; connection/verification failure and disconnect
release it. Background verification while offline does not lock preferences.
Changing aspect offline chooses the other already-verified identity on reconnect.
Old renderer lock APIs or helper identity schemas fail closed with an update
message. Matching aspect is required because the native widescreen camera still
changes guest camera state; one passing mixed-aspect match is not proof that all
future simulation paths ignore that state.

A room fixes the host's identity when created and repeats it in summaries,
detail acknowledgements, and Start. Mismatches fail before membership changes;
Ready and Start recheck all members. Identity and capabilities cannot change
inside a room. The native client independently checks the room acknowledgement
and Start against its verified loaded set before entering a lobby or CSS.

Existing v2 clients without this extension may still use rooms containing only
other legacy clients. They cannot join verified rooms. Current clients require
`compat-v1` from the server and do not silently downgrade verification.

This deliberately conservative gate compares executable bytes, including build
metadata, and the physical disc-image representation. Distinct executable
builds, or ISO/RVZ images with equivalent logical contents but different bytes,
will require matching distributions before joining a room. It is a compatibility
check for cooperating game clients, not remote attestation of untrusted clients.
Ongoing rollback state hashes remain necessary: matching inputs do not prove
that every runtime bug or future nondeterministic path has been eliminated.

`scripts/check_online_native_ui.py` hashes its selected executable once and
creates fixture hosts with the matching identity separately for each aspect.
`scripts/check_netplay_local.py` accepts `--host-widescreen` and
`--join-widescreen`; `--expect-aspect-reject` requires rejection before room entry
or gameplay. To validate an unpublished costume candidate, provide its exact
`--costume-package` SHA-256 and `--costume-package-file` ZIP. The bounded read is
hash-checked and strictly validated into a private fixture repository before any
native peer starts; installed author files and public publishing are untouched.
