# Workshop Lua API 1

Open a custom fighter in Melee Workshop and select **Lua**. Stock fighters must be cloned before editing. Choose or create a script, validate it, then save it. **Test in game** saves and launches the development game using the XML project settings. Select the custom slot on the original character select screen.

Scripts are plain UTF-8 `.lua` files inside the package's `scripts/` folder. The selected entry is recorded in `manifest.xml`. `require("moves")` loads `scripts/moves.lua`; dotted module names load subfolders. Blender scenes remain `.glb`, `.gltf` and `.blend`; behavior is stored in Lua beside those assets.

```lua
local fighter = { api_version = 1 }
function fighter.on_spawn(self)
    self:log("My fighter is ready")
end
function fighter.on_frame(self)
    local state = self:state()
    -- Use animation_frame for move events; frame counts gameplay callbacks.
end
return fighter
```

Each player gets a separate instance. `on_spawn` runs when that fighter instance loads. `on_frame` runs after native inputs and interrupt checks and pauses during hitlag and fighter freeze. Scene changes close the old instance. Module locals may hold per-player state. Runtime errors stop that player's script and appear with a `[lua] Pn` prefix in the game log. Syntax validation does not execute scripts or prove gameplay behavior.

## Fighter methods

| Method | Behavior |
| --- | --- |
| `state()` | Returns frame, port (1..4), motion, animation, animation_frame, x, y, facing, damage, stick_x, stick_y, held, pressed, airborne. |
| `players()` | Read-only snapshots of active players indexed by port, with position, damage, motion and airborne state. |
| `hitboxes()` | Read-only current hitboxes, including world position, size, damage and victim count. Positions reflect the last native collision update. |
| `stop_sound(channel)` | Stops a package WAV on channel 1..8. |
| `log(text)` | Writes to the game log. |
| `velocity(x, y)` | Sets self velocity in stage units per frame; also updates grounded velocity. |
| `facing(direction)` | Sets -1 for left or 1 for right. |
| `set_damage(percent)` | Sets 0..999 damage. |
| `animation_speed(rate)` | Sets the current clip speed, 0..8. |
| `motion(id)` | Enters a common Melee motion (0..340) with that state's native callbacks. |
| `animation(id, rate=1)` | Replaces the clip with an entry from this package's animation table. This does not change motion callbacks. |
| `interrupt(enabled)` | Enables the current native state's interrupt window. |
| `command(index, value)` | Sets a native motion command variable, index 0..3. For aerial attacks, command 0 controls landing lag. |
| `jab_transition(enabled)` | Enables the native jab follow-up window. |
| `hurtbox_state(state)` | Sets all hurtboxes: 0 normal, 1 invincible, 2 intangible. |
| `hitbox(spec)` | Creates or updates native fighter hitbox 0..3 using the fields below. |
| `clear_hitboxes(id)` | Clears one hitbox; omit id to clear all four. |
| `sound(path, channel=1, gain=1)` | Plays a package-relative PCM16 WAV, mono/stereo, 8..96 kHz, up to 8 MiB. Channels 1..8 replace their previous sound. Gain 0..2. Uses the game's volume/mute output. |

`hitbox` fields: id, group (0..7), bone (native rig index), damage, angle, growth, base, weight, size, x, y, z, element, shield, sound_level, sound_kind. Numeric 0/1 fields: clank, rebound, air, ground. Position is bone-local. Defaults are a 1-unit normal hitbox, angle361, growth100, damage0, both air and ground. Updating a live hitbox in the same group preserves its victim tracking; clear it before creating a new hit phase that should hit again.

```lua
self:hitbox { id=0, bone=23, damage=6, angle=80,
    growth=75, base=60, size=3, x=0, y=2, z=0 }
self:sound("audio/attack.wav", 1)
```

Imported rigs may provide `on_expression(self, channel, frame)` to receive native facial events. A custom rig's own facial assets must implement the visual change.

Inputs use Melee's button bits: A=0x100, B=0x200, X=0x400, Y=0x800, Start=0x1000, Z=0x10, R=0x20, L=0x40; D-pad left/right/down/up=1/2/4/8. For example `(state.pressed & 0x100) ~= 0` detects a new A press.

The VM includes base, table, string, math, utf8 and coroutine libraries, with a 32 MiB memory limit and a callback instruction budget. Filesystem, process, network, native module loading and arbitrary code loaders are not exposed. Package modules and WAV paths are validated. The gameplay API is under development; imported articles, projectile creation and complete Brawl move parity are not yet supported.
