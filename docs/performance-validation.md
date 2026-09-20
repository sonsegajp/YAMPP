# Four-player performance validation

Measured locally on 2026-09-19 with an Intel Core i5-10300H and NVIDIA GTX 1660 Ti Max-Q. The tested four-player match improved from 55.67 FPS to 59.39 FPS with a cold renderer cache and 59.54 FPS with the copied warm cache. A constant 60 FPS is **not** established.

## Native gameplay fixture

The driver uses ordinary controller inputs to select Falco against level-1 CPU Ness, Kirby, and Zelda on Corneria. Zelda can transform into Sheik through normal gameplay. The original character-selection and match code run throughout. Final saved player records confirm four active players and three CPUs.

Every compared run uses 1920 x 1080, widescreen, automatic resolution, VSync off, the same renderer DLL, the same input sequence, and an isolated memory card/settings directory. Automatic resolution uses the actual window pixel size; it does not round up to a higher fixed scale. The measurement covers host frames 2900 through 4800, approximately 31.7 seconds of an 80-second run. Compiles, other game tests, and Blender rendering were excluded from these measurement windows. Frame captures and the intrusive native CPU sampler were disabled.

FPS and intervals below are calculated from completed frame-packet submissions, not monitor scanout timestamps. These are single runs of one matchup, not a guarantee for every stage, fighter, CPU level, or machine. AI behavior and operating-system scheduling can vary between runs.

## Recorded results

| Build/cache | FPS | 95th-percentile interval, ms | Intervals >25 ms | Intervals >33.33 ms |
| --- | ---: | ---: | ---: | ---: |
| Original CPU path, cold cache | 55.67 | 23.51 | 72 | 22 |
| Matrix transfer, cold cache | 58.62 | 21.33 | 23 | 12 |
| Plus FIFO dispatch/copy, cold cache | 59.29 | 20.91 | 15 | 8 |
| Plus entry-hook layout, cold cache | 59.39 | 21.01 | 14 | 9 |
| Final build, copied warm cache | 59.54 | 20.77 | 12 | 3 |

The cold final run had 14 intervals longer than 25 ms versus 72 in the original CPU path. The small difference between the last two code revisions should not be treated as a proven performance gain. The warm repeat improves the average only slightly, so remaining spikes cannot be attributed entirely to first-use shader compilation.

### Host timing versus simulation and rendering

The warm run's host clock advanced at 59.999997 Hz: 1,900 tick intervals took 31.666668 seconds. The inclusive host-frame range contains 1,901 positions and 1,886 submitted frame packets; 15 positions had no packet and none had multiple packets. The complete run logged 4,800 host ticks and 4,800 retraces. Therefore 59.54 rendering FPS must not be described as a measured 59.54 Hz simulation rate.

The original match counter at `0x8046B6C4` ended at 2,332 in the warm run, compared with 2,333 in both final cold runs; their HUD timers also differ by only one frame. This supports near-identical total game progress. The existing CSV's `scene_ticks` field is zero for this fixture, and the exact native match-counter delta within the measured window was not captured, so exact simulation frequency for that window remains unproven.

Of the 12 warm-run submission intervals above 25 ms, nine consumed a packet within 0.1 ms of its readiness signal. The other three showed readiness delays of 5.28, 8.53, and 17.64 ms. Only one measured rendering-work sample exceeded 16.67 ms. These observations show intermittent late frame production or scheduling and occasional host-side delay; they do not isolate one timer or GPU cause. The major measured slowdown and stutter improved, while occasional presentation drops remain. Per-event evidence is `build/performance/four-player-final-warm/pacing-analysis.json`.

Local evidence is under `build/performance/`: each named run below contains `inputs.json`, `frames.csv`, `report.json`, and `run.log`. The aggregate is `four-player-validation.json`.

- `four-player-1080p-auto`: original CPU path.
- `four-player-matrix-after`: matrix transfer change.
- `four-player-fifo-after`: FIFO dispatch and copy change.
- `four-player-trace-after`: final cold build.
- `four-player-final-warm`: separate output seeded with the preceding cache.

Earlier capture runs and timing runs overlapped by unrelated work are excluded from this table. The separate native CPU sampling run is diagnostic evidence only and is not a timing comparison.

## Changes and parity checks

The matrix transfer wrappers eliminate generic quantization dispatch for the two SDK routines whose instructions specify raw float transfers. Float-to-double-to-float behavior, including signaling NaNs, register results, overlapping memory, and ordered MMIO accesses remain intact.

The disjoint FIFO MMIO range is registered first to avoid repeated failed device lookups. Ordinary command bytes are appended together when no display-list splice is pending; partial capacity behavior and the original split-command path remain intact. The old diagnostic CP byte scanner and its flush calls run only with `MELEE_TRACE_DIAGNOSTICS`. The GX diagnostic accumulator still receives the same bytes.

The function-entry hook keeps optional diagnostics and the rare ARQ register-save path outside its hot body, and uses one address switch with the original callback order. Service intervals, interrupt checks, complete register handling, and rollback behavior are retained.

All three differential suites passed with both Windows GCC and Linux GCC:

| Command | Evidence |
| --- | --- |
| `python scripts/check_sdk_matrix_transfer.py` | 98,304 cases: original generated instructions versus wrappers; full Context, RAM, ordered MMIO, floating-point edge cases and trace modes. |
| `python scripts/check_wgpipe.py` | 49,152 FIFO writes and 40,000 MMIO routes per diagnostics mode; pending/split lists, capacity and allocation failure, exact accumulator bytes/counters and address-width edges. |
| `python scripts/check_trace_entry.py` | 200,000 primary entries per diagnostics mode; full Context, seen map/counters, nested callbacks, asynchronous state changes, and every special hook address. |

Reports are `build/tests/{sdk-matrix-transfer,wgpipe,trace-entry}/report-{windows,linux}.json`. Test fixtures contain host source and synthetic data; no disc or mod payloads are required by these checks.

The measured executable (SHA-256 prefix `b87d50a1`) also passed the impaired Online regression: 89 matching confirmed state hashes, 73 gameplay samples through epoch 3/frame 4320, and 36 actual rollback corrections across 8,000 host frames. Both peers exited successfully without assertions, faults, or a state mismatch. That Online test used renderer SHA-256 `07bf462ec1fe4d0810c0a51e77e2702b401f075c70a890bd29a21ce9f14c2508` and the FD 1.0.2 package subsequently published and installed locally. It predates the presentation-only alignment renderer with hash prefix `454674`; neither that alignment renderer nor the Online-test renderer was used for the performance timings above. The Online record is `build/comparisons/yampp-fd102-wide-final/report.json`. See [Online validation](netplay-validation.md) for the full scope.

## Tested artifacts

- Executable: `build/native-game/yampp-four-player-final.exe`
  - SHA-256: `b87d50a1698f24811dbea4b2514573f5d00a91d4b7b0bfdfa36beb82211d70ab`
- Renderer used for every performance comparison: the archived local timing renderer, now stored at `build/backups/yampp-before-fd102-promotion-20260919-195727/melee_aurora_yampp.dll`.
  - SHA-256: `f5b551fe1f4061efc943163da7ddfd4516dad11a0e66d0576e30889e6f0f0783`
  - During these runs it occupied `build/pc/bin/melee_aurora_yampp.dll`; that current stable filename has since been replaced. The timing figures refer to the archived hash, not the later UI-alignment renderer.
- Renderer used for the recorded combined Online regression:
  - SHA-256: `07bf462ec1fe4d0810c0a51e77e2702b401f075c70a890bd29a21ce9f14c2508`

## Reproduce

Build the native executable and supply your own validated game extraction as described in [building](building.md). The commands below reproduce the recorded local comparison with its archived renderer; use a separately named run and record new hashes when testing a newer renderer. Keep other heavy tasks idle during the measured gameplay window.

```powershell
python scripts/profile_four_player.py --name four-player-cold --cpu-opponents --wide --width 1920 --height 1080 --scale 0 --executable build/native-game/yampp-four-player-final.exe --renderer build/backups/yampp-before-fd102-promotion-20260919-195727/melee_aurora_yampp.dll
python scripts/profile_four_player.py --name four-player-warm --cpu-opponents --wide --width 1920 --height 1080 --scale 0 --warm-cache-from build/performance/four-player-cold/cache --executable build/native-game/yampp-four-player-final.exe --renderer build/backups/yampp-before-fd102-promotion-20260919-195727/melee_aurora_yampp.dll
```

Use a separate run with `--capture` for visual inspection; readback changes the workload and invalidates an FPS comparison. Local captures, RAM snapshots, shader caches, extracted game data, and mod packages remain excluded from Git.

## Selective Akaneia runtime, September 20

Akaneia's m-ex patches caused texture updates and joint matrices to execute through the compatibility engine even for original fighters. Batched register transfers and exact, guarded native variants remove much of this overhead. Original scale-compensation behavior remains intact; unexpected patch bytes use the dynamic fallback. See [implementation and parity checks](akaneia-testing.md).

The four-player comparison below uses four human-controller slots driven by the same scripted attacks, 960x720, 4:3, render scale 1, VSync off, and host frames 2900-4200. All four native player records are active. This is a different fixture/resolution from the earlier 1080p Corneria CPU test and must not be directly compared with that table.

| Akaneia runtime | Rendered FPS | Median interval, ms | 95th percentile, ms |
| --- | ---: | ---: | ---: |
| Before this pass | 30.12 | 32.17 | 42.52 |
| Register transfers and texture variants | 46.01 | 20.58 | 26.96 |
| Plus complete matrix variant | 55.20 | 17.63 | 21.67 |
| Final timing run, optional CPU sampler disabled | 55.48 | 17.52 | 21.36 |

No captures, other game tests, builds or heavy analysis ran during these measurement windows. The first row used a cold isolated cache; later rows copied a previous run's cache. Measurements are well after loading, but this is not an identical-cache experimental control. The first three rows have the same observational guest sampler enabled; the final row disables it. The small 55.20/55.48 difference is not evidence of a significant sampler-related gain.

A separate two-player FD Link/Samus fixture improved from 50.22 to 59.96 rendered FPS after the texture optimization, with no measured intervals over 25 ms in its final sample. It does not establish four-player or all-fighter performance. Four-player rendering still falls short of a sustained 60 FPS in the tested scene. Rendering submissions are not monitor scanout timestamps or proof of exact simulation speed. No comprehensive Akaneia fighter/stage performance matrix has been completed.

Artifacts: `build/performance/akaneia-validation.json`, each named run's `report.json`, `inputs.json`, `frames.csv`; `build/performance/akaneia-native-textures.json` for two-player results. The older `akaneia-four-matrix` run is excluded because its address validator rejected the native variant; `akaneia-four-matrix-final` contains the corrected validator.


## Persistent-core four-player recheck, September 20

A clean A/B run uses the same executable/core/renderer, inputs, 960x720 scale-1 settings, and identical warm-cache seed. No other game, import, build, or capture ran during either measurement. All four native player records were active; both runs exited without an assertion or fault.

| Adjacent Gekko instruction batching | Rendered FPS | Median interval, ms | 95th percentile, ms |
| --- | ---: | ---: | ---: |
| Disabled | 54.27 | 17.70 | 22.17 |
| Enabled | 54.09 | 17.73 | 21.72 |

This shows no material whole-game improvement from batching in this fixture. Four-player Akaneia still needs optimization. The renderer end-frame stage accounts for about 5.9 ms on average, but the timings alone do not isolate its cause. Reports: `build/performance/akaneia-batch-clean-off` and `akaneia-batch-clean-on`.

The older `akaneia-batch-baseline` and `akaneia-batch-enabled` timings are explicitly invalid: another user game was still running. Their reports mark `timingValid=false`; they are not performance evidence.

The bounded instruction batch itself passes a full guest-state digest comparison with single-instruction execution (`c0297568` in both modes), repatched code, native entry boundaries, and batch-limit crossings. Its synthetic 2,000-call test took 0.160 s disabled versus 0.017 s enabled; this is not a whole-game speedup claim. The impaired Online rematch test also runs one peer in each mode and compares confirmed states. See [Online validation](netplay-validation.md).


## Immutable frame handoff, September 20

The guest previously waited for Aurora to finish processing the translated frame before it could start the next simulation frame. Texture palettes, vertex arrays and inline display lists have already been snapshotted at that boundary. Releasing the guest after translation overlaps its next frame with renderer processing; snapshots stay alive until the preceding frame drains. No simulation or rendering work is skipped. `MELEE_OVERLAP_RENDER=0` retains the serial path for diagnosis.

The matched A/B uses the same native executable/core/renderer, scripted four active human slots (Ness, Samus, Link, Pikachu), Flat Zone, identical warm cache, 1920x1080 widescreen, scale 4, VSync off, and host frames 3000-4500. Capture, CPU sampling, other games and build work were disabled during measurement.

| Guest release | Rendered FPS | Median interval, ms | 95th percentile, ms |
| --- | ---: | ---: | ---: |
| After renderer processing | 54.4443 | 17.7030 | 22.5700 |
| After immutable translation | 60.0006 | 16.8736 | 20.5787 |

Both exited cleanly with four active native player records. The Online regression ran one peer with overlap enabled and one disabled, through content consent/reload and two matches: 104 matching checkpoints, including 72 gameplay and 40 in the second match, with 24 rollback corrections and no detected divergence. Both matches were visually inspected. This establishes the exercised scene and synchronization routes, not every combination or monitor scanout timing.

Evidence: `build/performance/akaneia-four-overlap-off`, `akaneia-four-overlap-on`, and `build/comparisons/overlap-akaneia-rematch`.

The corrected Kirby/Game & Watch native fixture separately measured 60.0044 FPS with the original copy material and 60.0067 FPS without it at 1920x1080, widescreen, scale 4, over frames 3000-4200. Both selected Kirby/Game & Watch and exited cleanly. The reported intense slowdown did not reproduce in this scene; this is not a claim of a newly fixed shader defect. Evidence: `kirby-gw-copy-1080-clean`, `kirby-gw-plain-1080-clean`; visual capture is separate from timing.

## Tester r4 final bytes

After the palette-view correction and thread-safe music timer, the same isolated 1080p scale4 four-player Akaneia fixture measured **59.7177 rendered FPS**, median 16.8023 ms, p95 20.1491 ms, maximum 47.8589 ms over host frames 3000-4500. All four intended players were active; exit0, no fault/assertion, no capture/sampler or concurrent game/build. This is close to 60 with occasional slow frames, not a locked-60 claim. The earlier matched overlap A/B remains the evidence for the improvement from 54.44 FPS. Evidence: `build/performance/tester-r4-four-final/report.json`.
