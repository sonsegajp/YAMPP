# Online

Yet Another Melee PC Port (YAMPP) uses a lobby server to list rooms and relay inputs over one TCP
connection. Players do not need to forward ports. Both client and server use
protocol version 2 with the `rollback-v1` synchronization identifier; update
the relay alongside the game. Incompatible servers and clients are rejected
before they can join a session. Self-hosting instructions
and protocol regression checks are in [server/README.md](../server/README.md).

## Playing

Open **Online** from the original main menu. **Mod Browser** is inside this
submenu alongside room browsing, hosting, and disconnecting. The configured relay connects
when this screen opens. Choose **Browse Rooms** to see available rooms or
create a room as host. Creating a room opens a keyboard name dialog. Names
accept up to 47 printable ASCII characters; **Enter** or controller **A**
creates the room, and **Esc** or **B** cancels. **Backspace** deletes,
**Ctrl+A** clears, and **Ctrl+V** pastes accepted characters. Leading and
trailing spaces are trimmed; an empty name becomes `YAMPP room`.

The room browser shows room occupancy and actual room rules. Select a room
with the stick or D-pad and press **A** to join. **X** creates a room,
**Y** refreshes the list, and **B** returns. Full or active rooms cannot be
joined. The connection number is latency to the relay server, not a measured
round trip to that room's opponent.

Inside a room, **X** toggles your Ready state. The host can change the rules
and start once at least two players are ready. Changing rules clears Ready
for both players. **B** leaves the room. A completed or ended session keeps
the room available for a rematch. Character and stage selection use the
original game screens, with each peer controlling its assigned port.

The browser and lobby retain Melee's original animated frame and dark grid,
with a light-cyan menu theme, gold selection, and native player-panel styling. Their fields come from live session state. The
original menu background remains in use. Character select uses static portraits;
the experimental native 3D previews and Mod Browser model inspector were removed.
The browser loads the selected package's existing PNG preview through a bounded
background request, verifies its hash, and fits it in the static preview area.
Missing or rejected artwork shows **Preview unavailable**. Selecting a preview
does not download or activate the full package. Workshop still supports model
and animation inspection in its editor.

## Rollback implementation

Matches use the original Melee scene loop's simulation order through an
explicit callable frame step. Local inputs are delayed by the room's selected
number of frames. Missing remote inputs are predicted from earlier inputs.
The engine retains an eight-frame rewind window with nine snapshot slots.
Snapshots include CPU registers, game RAM, ARAM, audio state and deferred
device work. When a late input differs from its prediction, it restores the
corresponding frame and replays simulation with corrected input. Only the resulting frame is
presented. If confirmation falls beyond the eight-frame window, simulation
waits instead of overwriting required history.

Menus, character select, stage select, loading and results retain synchronized
input progression. Scene exits wait for confirmed input before unwinding the
native scene caller. State hashes are published only for confirmed frames.
Disconnects and confirmed state mismatches cancel the scene and return to the
Online menu instead of scoring the interrupted match as a tie.

Core bookkeeping is in `native/host/gxrt/rollback.c`. The runtime integration
is in `native/host/gxrt/netplay.c` and `netplay_rollback.inc`, based on the local
original `gm_801A4D34` reference. The new UI is rendered by
`native/host/aurora_shim/netplay_table.cpp`; menu input remains in `hooks.c`.

The host's rules are applied to both games. Session entry resets the temporary
VS selections through the original initializer; prior offline selections and
rules are restored when the session ends. Local saved name tags are hidden and
name entry is disabled during online sessions, keeping both CSS lists identical
without changing the saved name bank. A memory-card save made during a session can persist
the temporary rules until the next save.

## Verification

A successful compile or a prediction-only test is not proof of working game
rollback. Run the real game-pair check with delayed input:

```powershell
python scripts/check_netplay_local.py --name netplay-rollback-jitter --frames 8000 --input-delay 2 --relay-delay-ms 80 --relay-jitter-ms 20 --require-rollback --capture
```

This launches two isolated games and a temporary localhost relay. It requires
both peers to reach a match, identical verified compatibility fingerprints,
at least twenty matching confirmed gameplay
hash samples, no assertions or faults, and actual rollback corrections. The
relay logs injected latency so a test cannot silently pass without exercising
the impairment. Reports and captures are written beneath
`build/comparisons/netplay-rollback-jitter`.

For a normal local connection, omit the latency and jitter options. The test
relay exists only in `scripts/netplay_test_server.py`; production server
behavior is not modified to inject artificial delay.

UI checks are in `scripts/check_netplay_ui.py` and
`scripts/check_menu_additions.py`. `MELEE_CAPTURE_UI=1` includes the final
UI draw commands in diagnostic GPU captures; ordinary game-frame captures
do not contain renderer overlays.

## Compatibility and limits

- Both players need identical native executable and base-game bytes. The
  [compatibility gate](netplay-compatibility.md) verifies them before joining.
- Rollback accepts original fighters/stages and verified additive costume
  packages. Custom fighter replacements, Lua-driven characters and stages are
  blocked because their external state is not included in snapshots.
- A native guest-thread switch or scheduler-state change after a snapshot
  prevents a safe restore and
  ends the session instead of attempting to rewind an uncaptured native stack.
- TCP keeps deployment simple but packet loss can stall later input traffic.
  Rollback is bounded and cannot conceal an indefinitely stalled connection.
- Local automated tests do not establish performance over every Internet route
  or sustained gameplay with all stages and custom character packages.

Costume-only rooms advertise exact published package hashes. Guests see a
consent prompt before download or activation, and enter the lobby only after
the exact required set is verified and loaded. Stock costumes remain available;
custom scripts, stages and fighter replacements stay outside the supported
rollback set. See the [repository contract](mod-repository-contract.md).

The separately published Fierce Deity Link 1.0.3 costume pack adds teal, red,
green and violet colors after Link's five original slots. The current package
includes corrected face/eye textures, brown boots in every palette, and matching
static CSS portraits, stock icons and a clean Mod Browser render. Its original
Link geometry, UVs, skeleton and animations are preserved. See
[Workshop community](workshop-community.md) for package history and validation.

Workshop content is downloaded separately and is not bundled into the public
source repository or base distribution. See [validation](netplay-validation.md)
for the specific builds and game-pair checks completed so far.
