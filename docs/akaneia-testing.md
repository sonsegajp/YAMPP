# Selective Akaneia test build

Open `Run-YAMPP-Test.cmd` from this checkout. `Run-Managed-Test.cmd` and `Run-Akaneia-Test.cmd` select the same build. On Windows, the application, window, graphics device and controller connections stay open while the selected content reloads. Local settings and additive costumes are preserved. This is a development build, not a new distributable release.

## Content and consent

Only Akaneia 1.0.1 fighters, stages and music are imported: seven fighters, seventeen versus stages, seven corresponding target-test stages and forty-one tracks. The original YAMPP menus, base content and rules are retained. Volleyball and unrelated Akaneia modes/menu changes are excluded.

Open **Online > Mod Browser**, then press **X** for **Mod Manager**. Installed Akaneia appears as a content mod. **A** enables or disables it, with a brief "Applying mods..." view over the existing menu, then a return to the manager. This reinitializes game content inside the same process; it does not switch content during an active room or match. Additive costume packages use the same installed-mod page; FD Link's four added slots follow the stock Link costumes in both configurations.

When joining an Akaneia room without that content active, the game asks before downloading/preparing or enabling it. Downloads come directly from the pinned official GitHub release, never the YAMPP mod server. Preparation verifies the archive, patcher, local original disc and resulting content; the local importer retains only the requested content. After preparation, the persistent host reloads the correct game data, verifies the new compatibility identity, and retries the selected room in the same window. Joining an original-game room while Akaneia is active instead offers **Disable and Join**. Declining leaves the current content selection unchanged.

The same executable and reloadable game core, verified content, aspect ratio and costume set are required by both peers. Merely using an Akaneia version with the same displayed name is insufficient. Native fighter/stage/Lua packages outside this pinned content path remain experimental and are not covered by this Online validation. Profile pictures remain local in this tester.

No game data, Akaneia archives, prepared images, installed skins, personal state or generated game-derived code belong in the source repository or base distribution. A local portable candidate now includes an isolated Python/archive runtime and a self-contained content importer. A fresh official-GitHub download/import passed with developer Python/.NET absent from PATH and produced the same verified content hash as the development build. The Windows preview packages those tools with dependency notices and source references; the Akaneia archive and imported game content remain separate local downloads.

## In-process content reload (Windows)

Build with `python scripts/build_game.py --hot-reload --output-name YAMPP-hotload-test.exe --jobs 2`. The executable owns the renderer; its matching `YAMPP-hotload-test.core.dll` owns the guest game state. Both files are required. The current renderer is `build/pc/bin/melee_aurora_hotload.dll`. Launch through the CMD entry point so verified boot inputs and the local Python content helper are configured.

Before unloading a core, the runtime stops and joins guest, Online, music-decoder and optional profiler threads, flushes saves, releases input-file locks, unbinds UI callbacks, and frees guest memory and the execution engine. The renderer retains the window, device and input connections; guest texture state is invalidated before the next core starts. Its loading-image lifetime also covers diagnostic screenshot replay.

The plan helper verifies content before activation. Plans are parsed completely before environment changes; missing/duplicate fields are rejected. Failed preparation restores the previous enabled state, cancels the pending room join and returns to Mod Manager with an error. No download is authorized by a reload failure. The persistent host is Windows-specific; the existing standalone Linux build remains separate.

## Implementation

Original game functions are recompiled to native code. Added or modified PowerPC functions use a shared-memory execution engine, with native callbacks retaining menu hooks and device boundaries. Original Options hooks must also run when m-ex replaces their enclosing game function; missing those hooks caused the absent Widescreen row and old Controller hover illustration.

The performance pass batches register transfers without combining guest instructions. Exact, byte-guarded native variants handle m-ex's wider texture-palette index and complete joint scale-compensation patch. Any additional patch falls back to dynamic execution. The matrix variant is generated from Ploaj's m-ex assembly, including its relocated return addresses and custom scale compensation. Generated C and comparison bytes remain under ignored `build/` and require the user's verified original DOL.

Regeneration uses `powerpc-eabi-as` and `powerpc-eabi-objcopy` when the pinned m-ex source and devkitPPC tools are available. Without them, matrix execution falls back to the compatibility engine. Normal play does not start the optional CPU sampler; developer profiling uses `MELEE_SAMPLE_PATH` or `MELEE_SAMPLE_START`.

Rollback now recomputes the original off-screen/magnifier eligibility before each simulated step. Previously a render-produced flag stayed stale during replay, causing off-screen damage and then RNG state to diverge. Replay uses the original camera and eligibility helpers and does not require GPU draws.

## Validation and limits

The September 20 in-process reload build passed:

- Akaneia on -> off -> on with one process ID and one nonzero window handle, correct enabled/disabled screenshots, and clean exit.
- Native confirmation and exact-room rejoin in both directions, without process/window replacement.
- An actual public-relay match after the joining client enabled Akaneia: 34 matching state checkpoints, including 22 gameplay checkpoints through match frame 1260; 38 total rollback corrections at one frame input delay; no detected divergence, fault or assertion.
- Reverse-switch rollback match after disabling Akaneia: 52 matching gameplay checkpoints through frame 3060, 30 corrections, with 45 ms added latency / 15 ms jitter / one frame input delay; clean exits and no detected divergence.
- Deliberately failed preparation, restoration of the previous content, canceled join, visible recovery status and clean exit in the same window.
- Four plan/recovery tests, eight compatibility tests, production-host plan parsing and Windows argument roundtrips, the dynamic/native boundary regression, and 144 fast-path RAM comparisons.

These results cover the exercised routes and fixtures. They do not establish every fighter/stage combination or arbitrary long sessions. Evidence: `build/comparisons/content-hotload-final`, `content-hotload-recovery`, `content-hotjoin-akaneia`, `content-hotjoin-stock`, `hotload-akaneia-public`, and `hotload-stock-impaired`.

The following earlier tests document the preceding content/performance pass:

- Native Mod Manager disable/reload/return was exercised through the actual managed launcher.
- Native confirmation/reload/rejoin passed in both directions; screenshots show the correct connected lobby. Those room hosts are protocol fixtures, so these tests alone are not gameplay proof.
- Separate public two-client Akaneia gameplay passed 57 matching gameplay checkpoints through match frame 3360, with 22 rollback corrections and no detected divergence. These results cover the tested build and scenario, not all fighters, stages, routes or long sessions.
- The final runtime also passed an impaired two-client match with 45 ms added latency, 15 ms jitter and one frame input delay: 66 matching gameplay checkpoints through frame 3900, 35 rollback corrections, no detected divergence.
- The first teal FD slot was loaded through actual character select into a match alongside Akaneia; this is additional to costume-loader parity tests. Other palettes were registered but were not each replayed in this particular Akaneia pass.
- Dynamic-PPC/native boundary tests and 144 native-variant comparisons pass, including full RAM output, relocated hooks, custom scale compensation and fallback after unexpected patches. The differential fixture stubs helper callees consistently; actual gameplay capture is separate evidence.
- Relay unit tests: 74; compatibility tests: 7; official download/consent tests: 9; additive costume/catalog tests pass.

Local evidence: `build/comparisons/content-manager-cycle`, `content-rejoin-akaneia`, `content-rejoin-stock`, `akaneia-fd-final`, `akaneia-managed-public`, `akaneia-managed-impaired`; `build/tests/mex-fastpaths` and `mex-runtime`. Performance measurements and their limits are in [performance validation](performance-validation.md). No finite test suite establishes desync-free behavior for every possible match.


The longer results/rematch test and the latest clean four-player timing comparison are documented in [Online validation](netplay-validation.md#results-and-rematch-regression-september-20) and [performance validation](performance-validation.md#persistent-core-four-player-recheck-september-20). The rematch test passed. The newer immutable-frame handoff reaches 60 FPS in the matched four-player scene; see the final section of performance validation.


## Portable runtime validation, September 20

A relocated candidate was tested with PATH restricted to its own bundled binaries and Windows directories, an unavailable external DOTNET_ROOT, and Python isolated mode. The installed game's normal `play_yampp.launch` path prepared verified boot inputs and started the native executable/core/renderer. A diagnostic wrapper supplied finite-frame controller input only for these tests; the production launch has no replay input.

Both the original-content match and a match using the freshly downloaded/imported selective Akaneia image reached four active human-controller fighters and visibly rendered gameplay. Both exited cleanly after 3,600 host ticks without guest faults/assertions. These runs verify original fighters under both content configurations, not every added fighter. Evidence: `build/portable-game-validation/build/playback-v2-stock/report.json`, `playback-v2-akaneia/report.json`, and `portable-gameplay.jpg`. The fresh acquisition report is `fresh-import-report.json` in the same build directory.

The first stock replay omitted a menu confirmation; the first Akaneia replay reused a changed card. Those attempts did not reach gameplay and are excluded. The successful Akaneia run used the same isolated card fixture as the development test. Captures are identified by modification time after the corresponding launch, excluding older files with larger frame numbers.

Five portable-launch tests cover relocated paths with spaces/RVZ pointers, missing core, corrupt extraction, failed content preparation, immediate native startup failure, environment isolation, and preservation of user settings/state. The four content-plan tests additionally cover exact-room retry and failure recovery. This local validation does not publish a distribution or resolve the outstanding dependency-notice review.


All four installed FD 1.0.3 slots (5 through 8) subsequently passed native CSS selection and actual gameplay with Akaneia enabled in the rematch core. The replay uses the third-player cursor, which starts below Link in the expanded grid, and ordinary X presses. Each run verifies the active player is Link with the requested slot, logs the matching package load, and exits without fault/assertion. Captures visibly confirm teal, red, green, and violet clothing plus brown boots. Earlier horizontal-cursor attempts selected other fighters and are excluded. Evidence: `build/comparisons/fd103-akaneia-port3-slot-{5,6,7,8}/report.json` and `build/screenshots/testing-20260920/fd-akaneia-four-palettes.jpg`. This adds four-palette Akaneia gameplay coverage; it is not an all-stage or all-moves test.

The verified rematch core is now the local `YAMPP-hotload-test` build used by the normal test launchers. Artifact hashes and previous-build backup are recorded in `build/rematch-validation.json`. The local portable staging candidate is separate and has not been published or made into a new final ZIP.

On September 20 the user accepted the existing fighter/stage coverage and asked to stop expanding the exhaustive matrix. Unexercised combinations remain unverified; content audit results must not be represented as full move-by-move gameplay coverage.

## Supplemental stage files - September 20

The extracted m-ex stage metadata omitted runtime-loaded dependencies. The selective importer now follows stage archive/table references recursively and preserves their original filenames. Independent verification checks all 51 files for the 17 versus arenas and seven target stages against the official source, both in the composition directory and inside the exported ISO. This includes the Boxing Ring title CSV and Village scenery/visitor archives. Every source file in these stage namespaces is covered. Music remains independently hash-verified; menus, optional modes and codes remain excluded.

The former local import fails the new audit on `GrBxTt.csv`; the repaired import passes. Older local tester installations can disable and re-enable Akaneia in Mod Manager to rebuild with the corrected importer. New installations use it automatically.

Native two-player matches on the repaired Boxing Ring (Dedede arena, external stage 307) and Village (299) each completed 3,300 host frames with two active fighters, no assertion and no runtime fault. [Arena screenshot](screenshots/akaneia-boxing-20260920.png) and [Village screenshot](screenshots/akaneia-village-20260920.png) show the actual stage output. The user also confirmed the arena works. This is targeted validation, not an exhaustive moveset matrix.
