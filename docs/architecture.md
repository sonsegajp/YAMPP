# How YAMPP works

YAMPP is a PC port based on the [Melee decompilation](https://github.com/doldecomp/melee). The pinned decompilation supplies function identities, structures and the reference for original engine and menu behavior; YAMPP adds native platform services, Aurora rendering and PC features.

The input is Melee USA 1.02's `main.dol` and the files extracted from the player's disc. `scripts/regenerate_game.py` generates native C for the supported PowerPC program. The native executable runs that translated code against a 24 MiB guest address space; host modules implement platform services and selected hooks. Generated C is output: change the generator, hook configuration or runtime, then regenerate.

Aurora translates GameCube GX commands into modern graphics work. SDL supplies windows, input and audio. Melee's original menu scene, camera and animation remain in control; additional UI follows its transforms and uses fonts derived locally from the player's game. PC settings and the community browser supply host-side interaction through a versioned shared UI structure.

## Rollback

The relay organizes rooms and forwards input. Each peer runs the game locally. In matches the client predicts missing remote input within a bounded rollback window, saves guest RAM and the required host-side device/scheduler state, and re-simulates when late input differs. Re-simulation suppresses duplicate presentation effects. Confirmed state hashes detect divergence and stop an inconsistent session; they do not repair arbitrary incompatible game state.

Before room participation the client fingerprints its running executable and base-game files, keeps those files protected from modification, and verifies the exact immutable costume package hashes. The match begins from synchronized rules, selection state and input epochs. The server rechecks room requirements at Ready and Start. See [compatibility](netplay-compatibility.md) for the scope and limitations of those checks.

Cosmetic presentation must not change simulation memory according to whether a peer happens to render a frame. Static custom art uses reserved scratch storage and scoped texture substitutions; original pointers and scratch bytes are restored after their render packet is captured. Costume archives are validated and cached before play, so replay never reads mutable files from disk.

## Mods and Workshop

Workshop is an Electron desktop UI with an isolated Python worker over private pipes. A .NET/HSDLib bridge reads and writes HSD assets. Original extracted game files are read-only inputs. Edited content goes to separate `user/mods/<package-id>` folders.

Additive costume packs preserve stock fighter code, skeletons and animation banks. Their registry maps stable package/costume identities to additional color indices; stock indices stay first. Package hash order makes the mapping identical for peers. The native loader relocates a verified costume archive into guest RAM when requested and maps stock-only helper tables back to the costume's original base color.

The community service stores immutable, validated ZIP files by SHA-256. Clients derive download URLs from their configured repository origin and verify bytes before installation. A room names exact hashes, never a mutable latest version. The download consent screen appears before membership changes. Private publisher credentials and deployment configuration stay outside source control and packages.

Custom fighter and stage editing is a separate local workflow. It can change gameplay and is not accepted by the current cosmetic-only Online repository. Lua runs in a restricted per-player VM; see [Lua API](lua-api.md).
