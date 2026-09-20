# Additive costume index audit

Authority: the local GALE01 DOL and `upstream/melee/src/melee` source. Run
`python scripts/check_costumes.py` for the native loader regressions and a direct
comparison of all 26 playable fighters against the DOL's costume tables.

## Corrected cases

- **Falcon:** native slot 2 is red. The original filename starts as `PlCaRe.` and
  the game appends its language suffix (`ftCaptain/ftcaptain.c`). The catalog had
  omitted this dynamic filename, shifting white/green/blue down one position.
  `costume_order.json` now retains all six positions, using `PlCaRe.dat` for the
  stock Workshop source. Both `.dat` and `.usd` are present in the original files.
- **Kirby:** copy-hat filename arrays and caches have six entries, independent of
  the expanded body costume table (`ftKirby/ftkirby.c:2421,2738,2766`). Hooks at
  `800EEC34` and `800EED50` remap only hat color arguments to the original base.
  All-color prefetch remains six. `fn_80169C54` also has a seven-element stack
  list (six colors plus one selected color); its temporary enumeration count is
  bounded and restored after the call. Persistent custom player colors remain.
- **Classic enemy shuffle:** `fn_801695BC` uses `colors[6]`. Its native shuffle
  uses the original count and a remapped excluded base color, then restores the
  expanded menu count. This does not limit user-selectable costumes.
- **Zelda/Sheik and Popo/Nana:** both entities originally read one player color.
  Independently appended lists can otherwise select an unrelated transformed
  costume at the same ordinal, or fall back to color zero. The `80068914`
  initialization hook resolves only the secondary entity to the originating
  costume's stock base after initialization and before `Fighter_Create` loads
  the model. The primary entity and shared Player costume remain unchanged.
- **External count table:** there are 26 external records (`gm_1601.c`, size
  `0x68` at `803D51A0`), while the internal costume table has 33. External reads
  and writes are bounded to 26. The following bytes are results filenames and
  are now covered by a sentinel-preservation regression.
- **Registry identity:** native activation rejects mismatched external/internal
  fighter IDs/prefixes, duplicate pack/slot identities, conflicting pack hashes,
  and noncanonical pack ordering before replacing the active set. The Python
  generator orders by package SHA, package ID, then manifest-declared costume
  order. `community active` regenerates that registry after verification before
  returning the exact room hashes.
- **Game & Watch limits:** four native colors share one DAT. Installation limits
  count native slots instead of unique archive files.

## Existing protection and remaining scope

The body/item color lookups for Game & Watch are already remapped by the native
`8014A37C` and `8014A7F4` hooks. His stock model root contains no palette index;
the current costume manifest therefore infers base zero for a newly authored
Game & Watch DAT. Choosing one of the other three original palettes requires
explicit palette metadata, rather than inferring a distinct color from the
shared filename.

The focused native tests use real original DATs and the original DOL. They cover
slot allocation, original-color remapping, Kirby prefetch/load arguments,
transformed/partner collisions, rejection without partial activation, relocation,
and RAM replay. These are not a substitute for playing each fighter and mode in
the rebuilt game. Runtime hook configuration changes require caller regeneration.
