# Online validation - 2026-09-19

This public source repository records the checks below; prebuilt release availability is documented in the root README. Tests use isolated native executables and matching renderer DLLs. The existing relay, Workshop repository and validated artwork endpoint are deployed; this is separate from GitHub publication.

## Final tester r2 public-hostname build

Final executable `462d648eca41e379dbe952684c445beda1985abaf0b2e93c1173d5355ceee57e` and renderer `11bdb9505a4ada41c6c614264452a50a454f60599acbe2e8f232792bc75aca0f` passed the same 6,500-frame impaired-network tap-jump test after changing the public hostname to mmodx.fun: 68 matching confirmed hashes, 55 gameplay samples, 48 rollback corrections, both exits 0, no detected mismatch/desync/assertion/fault. Evidence: `build/comparisons/tester-r2-final-hostname/report.json`.

The relocated distribution also extracted the owned disc, verified the original DOL, launched the real native child through bundled-Python Workshop, rendered the public room browser and connected without private configuration. Its 2,400-frame run had no assertion or fault. The public HTTPS catalog and FD1.0.3 preview hash were checked under the new domain. Only nginx routing changed; the live relay code and Profile behavior remain unchanged.

## Earlier controller/Profile candidate snapshot

Executable `4c78ce74b40e0e88b354213921f2b15a04eb85515a3b711ace529882f2cd99fe` passed a 6,500-frame stock match with tap jump disabled for the host and enabled for the guest via synchronized inputs. Under 80 +/-20 ms delay and two-frame input delay, all 41 compared hashes matched, including 28 gameplay samples. There were 38 actual rollback corrections, zero detected desyncs/assertions/faults, and both peers exited normally. Evidence: `build/comparisons/tester-profile-tapjump/report.json`. This overlapped the user play window and is not a performance measurement.

All 65 relay tests passed. Actual local Profile save/reload and bidirectional avatar transfer passed against a disposable local relay. The user chose to leave the production relay unchanged; profile pictures remain local in this tester snapshot. See [tester checklist and limits](tester-build.md).

## Earlier FD 1.0.2 and widescreen validation

The final native pair completed original CSS, stage select and a real 16:9 Corneria stock match with corrected FD 1.0.2 art. Both peers ran 8,000 host frames in 139.31 seconds through an 80 +/-20 ms relay with two frames of input delay. All 89 comparable confirmed hashes matched, including 73 gameplay samples through epoch 3/frame 4320; the host corrected 15 predictions and the guest corrected 21. Both exited with code 0, with no detected mismatch, desync, assertion or fault. Actual captures verify the teal costume, static CSS portrait and three teal-cap/white-haired stock icons.

- Native executable SHA-256: `b87d50a1698f24811dbea4b2514573f5d00a91d4b7b0bfdfa36beb82211d70ab`.
- Renderer SHA-256: `07bf462ec1fe4d0810c0a51e77e2702b401f075c70a890bd29a21ce9f14c2508`.
- Tested FD package SHA-256: `048eaae4b7a9063d29ac06a4f2e42a123ddcf1cd0cfdf24cdbfd12b2521643b1`.
- Evidence: `build/comparisons/yampp-fd102-wide-final/report.json`, `artifacts.json`, `gameplay.jpg`, and `teal-stock-hud.png`.

After that gameplay run, the renderer received a presentation-only overlay correction. The current renderer SHA-256 is `4546749b715c10ad16fcfb797923e3352914216bdf8f5b722a3335959a2e6bb2`; the executable and published costume package are unchanged. Actual 4:3 and 16:9 Mod Browser captures verify the completed portrait, download consent and C-stick rotation with every overlay element inside the native frame. This later renderer was not used for the gameplay/performance measurements above. Evidence: `build/comparisons/fd-1.0.2-mod-browser-aligned/alignment-review.json` and the [screenshot gallery](screenshots/README.md).

Opposite-aspect clients were separately rejected before CSS/gameplay, with readable matching-build/data/aspect guidance. The aspect setting stays locked while connected and becomes editable after disconnect. A pre-guard mixed-aspect test also matched 64 gameplay samples with 26 rollbacks, but this bounded result does not establish universal camera independence; the compatibility guard remains. See [compatibility](netplay-compatibility.md) for the domain-separated runtime identities and byte-verification rules.

The widescreen runtime now keeps stored camera aspect canonical and widens perspective/texture projections once. Actual Fountain of Dreams captures verify aligned reflections without the central rectangular cutoff, while the 4:3 scene remains intact; Corneria's sky, distant landscape and stage geometry extend to both wide edges. Evidence is under `build/comparisons/fod-wide-after-late`, `fod-standard-after`, and `corneria-wide-after`.

## Earlier FD 1.0.1 gameplay checks

Two native instances completed original character select, stage select and a real Corneria match using Fierce Deity Link **1.0.1**, exact package SHA-256 `2200a1aa2b36df6f6aefe22b665bbb3c58771f2c32eaa1c233636e71bcce9393`. Each peer actually loaded internal Link kind 6, additive slot 5. The test requires those load records; earlier sequences selecting Zelda or Young Link were rejected as costume tests even when rollback hashes matched.

| Native build | Matching gameplay hash samples | Actual rollback corrections | Network conditions | Result |
| --- | ---: | ---: | --- | --- |
| `yampp-test.exe` | 63 through frame 3720 | 37 | 80 ms relay delay, +/-20 ms jitter, input delay 2 | Passed |
| `yampp-test2.exe`, bundled isolated Python | 54 through frame 3180 | 36 | Same induced delay/jitter | Passed |
| `yampp-final.exe`, final PNG/cache/UI integration | 32 through frame 1860 | 19 | Same induced delay/jitter | Passed |
| `yampp-final2.exe`, fresh-connect menu fix / packaged build | 54 through frame 3180 | 38 | Same induced delay/jitter | Passed |

All four runs completed the requested 8,000 host frames without a premature process exit. None reported a desync, hash mismatch, guest assertion or fault. The second compared 71 confirmed hash samples across menu/match epochs, including 54 gameplay samples. Its host shut down normally at the test limit and the remaining peer returned from the ended session.

The second executable SHA-256 is `a5fa61c0c11fee8fff4de0d4ffcbd9a28841ff364cf27ab978e9237b1431dfae`; its renderer SHA-256 is `0361f5bbd4515b8f12f746d82095d08c0427a0fc032de6d07e991d1ab31182ec`. The integrated `yampp-final.exe` SHA-256 is `989ee6cdddaf0d2ef2dbc957328f4ffcdf3296626ed5babe164813c07c54b30a`; its renderer SHA-256 is `1325eb243c4af781a96960a829edf02899194b5a37ddcde363c9b556b5139c3a`. That run compared 49 confirmed samples overall with 19 real corrections, and both processes exited with code 0. The packaged `yampp-final2.exe` SHA-256 is `6de05a38bba35ad0987cbba0bb0e0aac7c7d9882dd0f8076bcf1b675339eef1d`; its renderer is unchanged. This final run compared 71 confirmed samples overall, including 54 gameplay samples, with 38 actual corrections and both processes exiting with code 0. These results apply to the named files.

Local evidence: `build/comparisons/yampp-fd101-link41/report.json`, `build/comparisons/yampp-fd101-portable/report.json`, `build/comparisons/yampp-final-fd101/report.json`, and `build/comparisons/yampp-final2-fd101/report.json`, with actual gameplay images in each `host` directory. Test fast-exit is enabled after capture and card flush to avoid a known renderer DLL-detach shutdown fault; it does not suppress in-game errors.

## Earlier Linux-integration Windows checkpoint

The earlier `YAMPP.exe` / `melee_aurora_yampp.dll` checkpoint included the Linux PR integration and the source corrections recorded below. The strict 8,000-frame two-player test passed with 83 matching confirmed samples, including 67 gameplay samples through match frame 4020, and 36 actual rollback corrections under 80 ms relay delay with +/-20 ms jitter and input delay 2. Both peers loaded FD 1.0.1 Link slot 5; captured gameplay visibly shows the first teal costume with white hair and armor. There were no hash mismatches, desyncs, assertions or faults, and both processes exited with code 0.

Executable SHA-256: `1fd580b80665a9a98737806dddbf6fe8a610fb38d02b242e7b3091ae906febc8`. Renderer SHA-256: `f5b551fe1f4061efc943163da7ddfd4516dad11a0e66d0576e30889e6f0f0783`. At that checkpoint, the stable filenames were byte-identical copies of the tested `yampp-linux-merge-check.exe` / `melee_aurora_yampp_qa.dll`. Evidence: `build/comparisons/yampp-linux-final-fd101/report.json`, `artifacts.json` and `host/gameplay.jpg`; promotion hashes: `build/tests/yampp-integrated-promotion.json`.

## Focused checks

- All 57 server protocol, compatibility and repository tests passed after the costume-art extension. Public catalog and PNG readback returned the declared immutable bytes.
- The production costume loader preserves all stock counts, allocates additional packs without duplicate slots, rejects malformed/order/hash conflicts atomically, and reproduces byte-identical guest RAM after archive relocation/replay. Kirby copy-hat bounds and paired fighter forms are covered.
- Native portrait/icon tests check strict GXT format/dimensions/hash, immutable cached bytes, stock fallback and rejection without changing prior active RAM. CSS/HUD draw tests restore original texture pointers and the entire 24 MiB guest RAM after frame completion.
- The first teal FD slot was also verified offline in the unchanged final2 build using the live costume registry, copied user settings/save and normal controller input: select Link, then cycle X five times after the five stock colors. The actual match visibly shows teal clothing, white hair and armor. Evidence: `build/comparisons/fd-offline-live-slot5-link40/report.json` and its `offline-first-fd-gameplay.jpg`. That older build shared the stock-green portrait; FD 1.0.2 now supplies distinct per-color artwork, verified in the current checks above.
- FD 1.0.1 retains all 75 bones and 99 mesh records for every color, along with the original eye-animation track/key counts. All fourteen neutral/animated eye images now use corrected RGBA8 data. Workshop captured all four corrected colors without console errors. This does not imply every color has had a sustained multiplayer run.
- The final executable keeps all five Online rows through offline entry, connecting, disconnecting and reconnecting. Actual native UI checks also opened Rooms from offline and opened/canceled Host without losing rows.
- Room-name core tests cover 47 printable ASCII bytes, trimming/default names, Backspace/clear, cancellation and busy-command safety. Actual SDL text/Backspace/Enter created the exact named room observed by a separate client. UI captures check placement independently of core tests.
- Exact-hash room caches keep a required older published pack separate from a newer installed pack with the same ID. Production loader and packaged-worker checks cover online selection, offline restoration, reuse without network, and rejection after extracted-DAT tampering. Installed packages are unchanged.
- The relocated distribution passed cold Online activation and public PNG retrieval without game data or the HSD bridge. After adding private extracted fixtures only in the test folder, the actual Workshop catalog and stock Link inspection passed with the relocated bridge and no stderr. The RVZ Test Game argument check was mocked; it did not launch gameplay.
- The isolated bundled Python worker returned the actual 26-fighter, 120-costume catalog while ignoring deliberately invalid ambient Python paths. Native Online compatibility and costume verification also passed with that interpreter.

## Presentation and remaining limits

Live 3D previews remain reverted. FD 1.0.2 now has completed static portraits, stock icons and a clean Mod Browser image, produced from the actual corrected model in Blender using the original Link artwork as the pose/lighting reference. The user authorized this workflow after the image-generation failures. Native testing selected all four costumes through ordinary controller input and verified matching portraits and gameplay palettes across successive matches: `build/comparisons/fd102-four-colors/report.json` and `all-colors-native.jpg`. That offline run used native time rules. The final stock-mode Online pair above separately verifies the new teal HUD icon and rollback behavior. The exact tested 1.0.2 package is now published through Workshop and installed locally. Public ZIP/PNG readback matched the immutable package and preview hashes; the previous installed package is retained in a local backup.

Current UI work includes dark-gray headings, cyan Online frames, native menu transforms, original GameCube controller/button cutouts, room-name typing and a static PNG preview in Mod Browser. Actual 4:3 and 16:9 captures cover room naming, browser/lobby/rules, Mod Browser PNG/download consent, monitor selection and C-stick rotation. The room-name test also verifies a 47-character name, tail scrolling/caret and an ellipsized lobby heading. See the [screenshot gallery](screenshots/README.md).

These are local two-player tests with induced network impairment. They do not establish every Internet route, all stages/fighters, four-player rollback, cross-CPU determinism or subjective audio quality. Confirmed hashes detect divergence; no finite test suite guarantees all desyncs are impossible. Gameplay-changing fighter/stage/Lua packages remain outside supported Online sessions because their full custom state is not snapshotted. Custom local music is disabled during synchronized play.

Reproduction commands and controls are in [Online](netplay.md), [compatibility](netplay-compatibility.md), [costume audit](costume-index-audit.md) and [building](building.md).

## Linux integration checks

PR #1 has been integrated locally from `e8b2a57789956d2d7f33e2f4f2e7ec502298a3f0`, preserving the newer Online, rollback and costume changes. Windows GX, rollback and scheduler checkpoint regression tests passed after integration. WSL Ubuntu also passed GX, rollback, scheduler checkpoint and platform-compatibility tests; the latter includes 10,000 real event handoffs, auto/manual reset behavior, 32-bit Win32 type widths and bounded waits.

The real Linux native community worker passed UTF-8 and shell-punctuation argv handling, cancellation/reaping, file-change detection, descriptor-limit handling and a catalog request through Python. Production Linux costume loading passed stock preservation, archive relocation, exact sets and artwork validation. Both the full Linux runtime and Aurora renderer compile and link on Ubuntu 26.04 LTS / GCC 15.2.0 under WSL2. The actual boot probe loads the original DOL, then the existing hardware-Vulkan guard rejects WSL's llvmpipe software adapter. No Linux gameplay, visible output or audio is claimed. Evidence: `build/pr1-linux/integration-report.json` and `build/pr1-linux/boot/report.json`. The final Linux runtime and renderer also compile after the matrix/FIFO/entry optimizations, aspect guard, friendly costume labels and overlay alignment correction. The Linux atomic aspect-lock test passes 5,000 concurrent acquire/edit races, preserves offline preferences and rejects edits while locked. Windows/Linux cross-play remains rejected by the exact executable compatibility check.

Final Linux artifacts are `build/native-game-linux/yampp-wide-final` (207,911,704 bytes; SHA-256 `abddd25b6f57bcab09135e8449ecf97313f5002ede8759a60c971b51dda8c250`) and `build/pc-linux/libmelee_aurora_wide.so` (22,395,064 bytes; SHA-256 `01e93f702fda49c66665f20411af58f5897f9830c3582a57643c933c1f010ad6`). Evidence: `build/tests/linux-wide-final-build.json`. These are compile/link and focused-test results; the hardware-adapter limitation above still prevents a Linux gameplay claim.

The production Linux FFmpeg decoder also passes mono/stereo PCM comparisons at multiple sample rates, malformed input and allocation-failure checks. A discovered capacity/flush bound issue was fixed: both the regular decoder and final flush clamp frames and allocation to the same existing limit. Small-cap production-code tests verify the exact output prefix without oversized allocations. Evidence: `build/tests/music-linux/report.json`. Audible custom music remains unverified on Linux.


## Selective Akaneia regression, September 20

The pinned fighters/stages/music-only build now participates in exact content compatibility. Confirmation plus managed reload/rejoin was verified in both directions (enable for an Akaneia room, disable for an original-game room), including sustained connected lobby screenshots. Akaneia is acquired from official GitHub only after confirmation; no mod archive is mirrored by the relay.

A separate public two-native-client match passed 70 matching state checkpoints, including 57 gameplay checkpoints through match frame 3360 and 22 corrections. The final build (after making the optional CPU sampler opt-in) passed injected 45 ms latency / 15 ms jitter / 1F input delay: 79 matching state checkpoints, 66 gameplay checkpoints through frame 3900 and 35 corrections. Neither run detected divergence, assertion or guest fault. These are tested scenarios, not a guarantee covering every added fighter/stage or Internet route.

This pass fixed an actual rollback defect: off-screen damage depended on a flag produced only during drawing. Replay skipped that draw and advanced the damage timer differently. Each synchronized step now derives the original visibility/eligibility using native game helpers. Exact compatibility still rejects differing executable/content/aspect combinations. See [selective Akaneia testing](akaneia-testing.md) for implementation and artifacts. Profile pictures remain local; that protocol extension was not deployed.

## In-process content changes, September 20

`build/comparisons/hotload-akaneia-public/report.json` records two actual native clients through the public relay. The joining client started with original content, received the official-GitHub consent prompt, enabled the installed verified Akaneia content in the same process/window, joined the requested room, and reached rollback gameplay. All 34 shared state checkpoints matched, including 22 gameplay checkpoints through frame 1260; 38 corrections occurred with one frame input delay. Both clients exited cleanly without fault, assertion or detected desync. The archive was already installed for this test; a fresh Internet download is covered separately by the download/consent tests, not claimed by this run.

`content-hotjoin-akaneia` and `content-hotjoin-stock` additionally test sustained room entry against protocol fixtures in both directions. `content-hotload-recovery` deliberately rejects a preparation plan and confirms restoration of the previous content and cancellation of the pending join. Compatibility now hashes and locks the reloadable game-core DLL as well as the persistent executable and existing content inputs.

The reverse switch also passed actual two-client rollback gameplay through an isolated relay with 45 ms added latency, 15 ms jitter and one frame input delay: 65 matching state checkpoints, 52 during gameplay through match frame 3060, and 30 total corrections. The joining client disabled Akaneia within the same process/window before entering the stock room. Both peers exited cleanly; no detected mismatch, fault or assertion. Evidence: `build/comparisons/hotload-stock-impaired/report.json`.


## Results and rematch regression, September 20

A longer run exposed a real scene-transition defect: local save-data unlock/prize screens could interrupt Online results, then reuse the previous synchronization epoch. Online results now use the original results routine's branch that defers local unlock/prize checks while retaining match records and normal return behavior. Offline results retain their original code path. The synchronized scene driver also rejects entry without a fresh scene epoch.

`build/comparisons/akaneia-rematch-native-stage/report.json` passes the full native character select -> stage select -> match -> results -> character select -> stage select -> second match sequence on both peers. The guest first enabled verified Akaneia after consent and rejoined within the same process and window. With 45 ms relay delay, 15 ms jitter, and one-frame input delay, all 102 shared state checkpoints matched, including 69 gameplay checkpoints and 37 in the second match. There were 52 total rollback corrections, no reused hash labels, no assertions/faults/unexpected session endings, and both processes exited normally. Sparse GPU captures visually verify the two matches and results screen. One peer used adjacent-instruction batching; the other used the single-instruction path.

The first attempt after the results fix returned correctly to character select, but its replay did not move the stage cursor for the second selection; it failed the rematch requirement and is not counted as a pass. The final replay repeats native stage cursor movement and confirmation. The test now explicitly fails reused epoch/frame hash labels.

The public TLS lifecycle recheck also passes create, exact-room join, leave/rejoin, Ready/start, and disconnect cleanup. `build/online-rooms-20260920-final.json` is transport evidence; the actual rematch regression above used a local impaired relay. Neither result establishes all fighters, stages, Internet routes, or unlimited session duration. Profile photos remain local.


## Rendering overlap and rematch regression, September 20

`overlap-akaneia-rematch` exercises host render overlap enabled and joiner overlap disabled. The stock joiner explicitly accepts Akaneia, reloads and rejoins in the same process and window. Both clients run CSS, stage select, match, results, CSS, stage select and a second match with 45 ms added relay latency, 15 ms jitter and one frame input delay.

The run exited cleanly with 104 matching state checkpoints, 72 during gameplay, 40 during the second match and 24 rollback corrections. There were no detected hash mismatches, reused checkpoint labels, assertions, faults or unexpected session endings. Actual captures show gameplay in both matches and the native intervening stage-select screen. This is a finite regression, not a guarantee against all future desyncs.

## Final palette-fix candidate, September 20

The final `YAMPP-palette-final` native pair and rebuilt music-overlay renderer repeated the impaired Akaneia consent/rejoin/rematch route. Both peers exited normally with 104 matching checkpoints (72 gameplay, 40 second-match), 27 rollback corrections, no mismatch/reused epoch labels/fault/assertion, and the same process/window across the joining player's content change. Host rendering overlap was enabled, join overlap disabled, with 45 +/-15 ms relay impairment and one frame input delay. Evidence: `build/comparisons/tester-r4-final-akaneia/report.json`. The remote relay and local-only Profile behavior were unchanged.
