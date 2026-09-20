# YAMPP Windows tester build - 20 September 2026

This is an experimental Windows preview; see the [public release notes](releases/2026-09-20.md). Extract the entire ZIP to a writable folder. Run **Setup.cmd**, select your own NTSC-U 1.02 Melee disc image, then open **Play YAMPP.cmd**. Normal launch plays the original opening FMV, then reaches the native title screen; A/Start skips the FMV as in Melee. **Melee Workshop.cmd** opens the included editor. No game data, mods, saves or personal settings are included.

## Changes in this snapshot

- The Melee-inspired **Yet Another Melee PC Port** logo is included in the README and `assets/branding`. The original in-game title animation remains.
- Optional Akaneia content includes **fighters, stages and music only**. Its original menu/mode modifications are excluded. Acquisition uses the pinned official GitHub release; YAMPP does not host or bundle Akaneia.
- **X** switches Mod Browser to Mod Manager. Enable/disable Akaneia there; content reload keeps the same application and window and returns to the menu. Joining a room with different required content asks for confirmation before downloading/enabling it, then rejoins the room after verification and reload.
- Four-player Akaneia rendering improved from **54.44 to 60.00 FPS** in the matched 1080p, scale4, Flat Zone fixture. The final packaged candidate recheck measured **59.72 FPS**, with occasional slow frames. These are measured scenes on one machine, not a guarantee for every matchup. See [performance evidence](performance-validation.md).
- Online results now return to synchronized character select for rematches. The impaired Akaneia consent/rejoin/two-match check passed 104 matching checkpoints, including 72 gameplay and 40 second-match samples, with 24 rollback corrections. See [Online evidence](netplay-validation.md).
- Distant Adventure ReDead palette corruption is fixed: separate texture units now retain their own RGB565/RGB5A3 interpretation. Native before/after captures and a reproducing regression verify the fix; see [rendering evidence](rendering-validation.md).
- Great Bay's alternate track displays **Saria's Song** in its actual in-game banner.
- Options > **Controller** replaces Screen Display and uses a flat hover icon on the original animated layered panel. Connect/Disconnect have consistent lowercase c glyphs. Widescreen and Controller art also remain active with Akaneia enabled.
- Controller mapping shows live buttons, sticks and triggers. **Z** toggles per-port tap jump; save to retain mappings/preferences. Official Nintendo/Mayflash four-port support is implemented, but physical adapter validation is still outstanding. Set Mayflash to Wii U mode.
- Online > **Profile** supports a typed username and PNG/BMP picture. **X saves**, **Y restores the default controller**, and **B discards edits**. Enter accepts typed text; Escape cancels it. Pictures remain local for this tester build. Reconnect after changing your username to apply it to a public session.
- Adventure's Underground Maze no longer preloads every additive Link costume for enemies, fixing the reproduced heap assertion/black screen. Maze loading and gameplay pass; a full Adventure clear has not been established.
- Fierce Deity Link **1.0.3** is a separate Mod Browser download, with four additive slots, brown boots across palettes, matching portraits and stock icons. All four were exercised with Akaneia enabled.

## Useful tester checks and known limits

1. Both Online players should use this exact build and the same aspect ratio. Create a named room, join it, ready up, play and rematch. Test both accepting and declining required-content downloads. Profile pictures remain local.
2. Check a physical Nintendo/Mayflash adapter: every port, sticks, analog triggers, buttons, tap jump and rumble. Virtual-port checks do not establish hardware behavior.
3. Play Adventure through Underground Maze and its exit with stock and FD Link. Check the corrected distant ReDead skin as the camera moves.
4. Report four-fighter frame drops with exact stage/fighters, resolution, render scale and machine specifications. The severe Kirby/Game & Watch copied-material slowdown did not reproduce in the tested 1080p fixture; broad or sustained coverage is not claimed.
5. Linux sources build and core tests pass, but actual Linux gameplay/audio and Windows/Linux cross-play are not verified. Cross-platform peers are currently rejected by exact executable compatibility checks.

The user accepted the exercised Akaneia coverage; an exhaustive fighter/stage/move matrix was not completed. General gameplay-changing Workshop mods remain outside the supported Online package contract. Physical hardware testing, long sessions and full Adventure progression remain explicit limits.

For a bug, include the screen/stage, fighters/costumes, input device, resolution/aspect and reproduction steps. Keep your private settings, personal pictures and disc files out of public reports.

Online and Workshop use the public `mmodx.fun` hostname. Private deployment configuration is excluded. Public source and Windows downloads are available at [YAMPP](https://github.com/sonsegajp/YAMPP).

The current audio update fixes ADPCM header-boundary corruption heard in Akaneia GameCube music. Actual GameCube and Green Hill audio checks pass the targeted waveform/decoder checks; occasional long-load playback gaps remain. See [audio validation](audio-validation.md).

Preview 2 changes initial Akaneia installation to a user-supplied archive in `user/imports`. Confirmed room joins can still acquire a missing archive directly from official GitHub. Follow [the installation guide](akaneia-install.md). The update passed 13 acquisition regressions, a real official-archive import/cache check without an archive download, and the native local-install UI capture.
