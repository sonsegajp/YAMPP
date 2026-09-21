# Online

Yet Another Melee PC Port (YAMPP) uses a lobby server to list rooms and relay
inputs. Players do not need to forward ports. Both client and server use
protocol version 2 with the `rollback-v2` synchronization identifier; update
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
joined. In the browser the connection number is latency to the relay server.
Once a match starts it becomes the measured round trip to the opponent, which
is the number the input delay and the clock correction are derived from.

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

## How input reaches the other player

Input takes whichever of two paths is working, and the in-match readout says
which: `direct` for datagrams, `relayed` for the lobby stream.

The lobby connection always works, because it is a port the server already
answers on, and it is what carries rooms, rules and match setup. It is a poor
carrier for input, though, because it is a stream: one lost segment holds back
every input queued behind it until the retransmission arrives. What a player
feels is a freeze and then a burst of rollbacks, on a link that never actually
stopped working.

Rollback input does not need ordering. Every packet names the exact frames it
carries and repeats the previous few, so losing one costs nothing -- the next
packet, about sixteen milliseconds later, already contains it. So the client
also opens a datagram channel to the relay, registers on it with a token the
server issued over the stream, and moves input onto it as soon as packets are
seen arriving. The address the datagram arrives from is where the server sends
that player's traffic, so NAT mappings keep working without port forwarding,
and a mapping that is renumbered mid-match simply re-registers.

If the datagram channel never works -- a blocked port, a hostile middlebox --
nothing is lost: the client keeps using the stream and only the probes are
wasted. If it stops working mid-match, the client notices the silence within
about a second and moves back to the stream by itself.

## Input delay and keeping the two clocks together

Input delay defaults to **automatic**. Inputs travel client -> relay ->
client, so the trip the delay has to cover is half of each client's round trip
to the relay. Each client reports that measurement with its lobby ping, and
the relay resolves one number -- one to six frames -- and sends it to both
players in the same start message. The rollback window absorbs the jitter on
top of it, so the delay only has to cover the steady state; anything larger is
latency the players feel for nothing. A fixed delay of one to ten frames can
still be set in the room rules, and is passed through untouched.

Both players must run the same delay. A match seeds the frames before the
delay as known-empty input on every port, so two clients disagreeing about
where that boundary sits would contradict each other the first time somebody
held a direction on frame zero. That is why the number is resolved once, by
the relay, instead of being measured independently on each side.

Two machines never tick at exactly the same rate. One display runs at 60.000
Hz and the other at 59.940, a background task steals a vsync, a laptop drops
to a power-saving clock. Half a hertz of difference is thirty frames of drift
a minute, far past the eight-frame rollback window, and long before any
timeout fires it is felt as constant micro-stutter and inputs that seem to go
missing -- because the faster machine ends up waiting on the network every
single frame.

The correction is GGPO's, which Slippi also uses. Each peer continuously
measures how far *behind* the other it believes itself to be, reports that
number in every input packet, and the two compare. When both agree on who is
ahead, the peer that is ahead gives back half the difference by letting that
many host vsyncs pass without advancing the simulation. Splitting the
difference makes the two converge rather than overshoot past each other, and
requiring both sides to agree is what stops them from both waiting for each
other. Corrections are larger while the match settles and then one frame at a
time, below perception. The decision uses half a second of averaged evidence,
so a single late packet cannot cause a hitch.

`native/host/gxrt/timesync.h` holds the measurement and the decision;
`native/host/gxrt/tests/timesync_test.c` runs two peers on genuinely different
host clocks through the same exchange and requires them to stay locked.

## When something goes wrong

A hiccup is not a disconnect. Losing the opponent for a moment **holds** the
match instead of ending it: the frame, the snapshots, the rollback window and
the lobby session all stay exactly as they were, the overlay says what is
happening and counts down the remaining grace, and play resumes from the same
frame the instant input arrives again. Only a link that stays dead for the
full grace period ends the session.

- **The opponent goes quiet.** The match is held for up to 45 seconds and
  resumes by itself.
- **Your own lobby connection drops.** The client reconnects underneath the
  players and rejoins the same session with `resume`; the server keeps the
  player's place in the match for `--hold-seconds` (40 by default) and tells
  the other players it is holding rather than that the session ended. The
  aspect lock and the verified game files are kept across the reconnect,
  because neither may change mid-match.
- **The opponent's connection drops.** Their side of the above; the overlay
  says they are reconnecting and for how much longer.
- **A correction cannot be applied** -- a snapshot has aged out, or a native
  scheduler change makes a restore unsafe. The correction is declined and play
  continues. The simulation in front of the player is still a valid
  continuation of the game; it simply used a prediction that turned out to be
  wrong, exactly as it would have with a slightly larger window. The count is
  logged at the end of the match.
- **A confirmed state mismatch (desync).** Recorded, logged in full and shown
  on screen, but never acted on. The two games have stopped agreeing, and the
  players are far better placed than the program is to decide whether the game
  still counts. Slippi does not detect desyncs at all; this reports them and
  keeps playing.

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

Pads are sampled on an input clock that runs faster than the simulation, so a
frame takes the *union* of what happened during it rather than whichever
sample landed last. Physical button edges are retained until the next forward
input sample, so a short tap during replay is not lost, and analog triggers
are carried at their peak for the same reason -- without that, a squeeze that
crossed the shield or air-dodge threshold between two samples was silently
swallowed. Releases and stick positions use current controller state: a stick
is a position, not an edge, and holding its extreme an extra frame would
lengthen every dash and smash. The physical sample is merged into whatever the
guest's own pad alarms already observed for that frame rather than replacing
it, which is where taps used to go missing.

Socket writes run on the network thread through a bounded queue; the
simulation thread does not sleep waiting for TCP/TLS sends. That thread waits
on its sockets with millisecond granularity rather than sleeping on a timer,
so an input handed over by the simulation leaves on the same pass and an
arriving packet is picked up as soon as the socket has it. Stalled frames
request missing input again and resend recent history without extending the
prediction window.

Online audio advances by one fixed 60 Hz slice per simulation frame. Replay
restores the simulated audio state without submitting duplicate sound to the
speaker. Playback drops stale queued output above about 83 ms and clears the
queue when changing synchronized scenes or returning to offline play. This
bounds accumulated delay; it does not eliminate short gaps caused by a long
render or network stall.

Menus, character select, stage select, loading and results retain synchronized
input progression. Scene exits wait for confirmed input before unwinding the
native scene caller. State hashes are published only for confirmed frames. A
disconnect that outlasts its grace period cancels the scene and returns to the
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

## When the Online menu cannot do anything

Room browsing, costume verification, Akaneia downloads and the compatibility
gate are all carried out by a helper process running the installation's own
bundled Python. If that helper cannot start, each of those actions fails with
its own message -- "Could not open the costume library", "Could not verify
installed costumes", "Could not start the GitHub download" -- and, because
Ready needs verified costumes, the lobby connects but no match can begin.

By far the commonest cause is an installation that is missing files: a
download that was still extracting when the game was launched, or a partial
copy. The runtime now checks for the interpreter, the helper script and
`scripts/project_config.py` before spawning, names the missing one and where
it looked in the log, and says **"Installation is incomplete. Extract the
whole download again, then retry."** The helper's own output, including any
Python traceback, is appended to `build/netplay-community/worker-<pid>.log`
next to the installation.

## Compatibility and limits

- Both players need identical native executable and base-game bytes. The
  [compatibility gate](netplay-compatibility.md) verifies them before joining.
- Rollback accepts original fighters/stages and verified additive costume
  packages. Custom fighter replacements, Lua-driven characters and stages are
  blocked because their external state is not included in snapshots.
- A native guest-thread switch or scheduler-state change after a snapshot
  prevents a safe restore. The correction is declined and play continues with
  the prediction already on screen; the stack is never rewound uncaptured.
- Input prefers datagrams and falls back to the lobby stream. On the stream,
  packet loss can still stall later input traffic. Rollback is bounded and
  cannot conceal an indefinitely stalled connection.
- The grace periods buy time for a link to recover; they cannot recover a
  match whose opponent has genuinely gone.
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

Confirmed mismatches remain reported. Both peers log the mismatching frame's
RNG, fighter state fields and inputs with the `[netplay-desync]` prefix, and
the in-match overlay says the two games have diverged. These logs distinguish
actual simulation divergence from a temporary connection stall. The match is
not stopped: a diverged game is usually still playable, and whether it still
counts is the players' call, not the program's.
