#!/bin/sh
# Launch Melee PC natively on Linux.
# Usage: ./run-melee.sh [path-to-iso]
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [ -n "$1" ]; then
    ISO="$1"
    shift
elif [ -n "$MELEE_DISC" ]; then
    ISO="$MELEE_DISC"
else
    # Look for an ISO in the data directory
    for f in "$SCRIPT_DIR"/data/*.iso "$SCRIPT_DIR"/data/*/*.iso; do
        [ -f "$f" ] && ISO="$f" && break
    done
fi

if [ -z "$ISO" ] || [ ! -f "$ISO" ]; then
    echo "Usage: $0 <path-to-melee-iso>" >&2
    echo "Or set MELEE_DISC=/path/to/game.iso" >&2
    exit 1
fi

# Verify GALE01 revision 02 (NTSC 1.02)
GAME_ID=$(dd if="$ISO" bs=1 count=6 2>/dev/null)
DISC_REV=$(dd if="$ISO" bs=1 skip=7 count=1 2>/dev/null | od -An -tu1 | tr -d ' ')
if [ "$GAME_ID" != "GALE01" ] || [ "$DISC_REV" != "2" ]; then
    echo "Error: disc image is not GALE01 rev 02 (NTSC 1.02 Super Smash Bros. Melee)" >&2
    echo "Found: ID=$GAME_ID rev=$DISC_REV (expected GALE01 rev 2)" >&2
    exit 1
fi

# Verify extracted DOL matches expected NTSC 1.02
DOL="$SCRIPT_DIR/data/GALE01/sys/main.dol"
EXPECTED_DOL_SHA1="08e0bf20134dfcb260699671004527b2d6bb1a45"
if [ -f "$DOL" ] && command -v sha1sum >/dev/null 2>&1; then
    ACTUAL=$(sha1sum "$DOL" | cut -d' ' -f1)
    if [ "$ACTUAL" != "$EXPECTED_DOL_SHA1" ]; then
        echo "Error: extracted DOL does not match NTSC 1.02 (SHA-1 mismatch)" >&2
        echo "Expected: $EXPECTED_DOL_SHA1" >&2
        echo "Got:      $ACTUAL" >&2
        exit 1
    fi
fi

export MELEE_DISC="$ISO"
if [ -f "$SCRIPT_DIR/build/pc-linux/libmelee_aurora_mod.so" ]; then
    export MELEE_AURORA_DLL="$SCRIPT_DIR/build/pc-linux/libmelee_aurora_mod.so"
else
    export MELEE_AURORA_DLL="$SCRIPT_DIR/build/pc-linux/libmelee_aurora_v2.so"
fi
export LD_LIBRARY_PATH="$SCRIPT_DIR/build/pc-linux:${LD_LIBRARY_PATH:-}"

# Persistent save data
USER_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/melee-pc"
mkdir -p "$USER_DIR"
export MELEE_MEMORY_CARD="${MELEE_MEMORY_CARD:-$USER_DIR/MemoryCardA.raw}"
# MELEE_MODS is left unset unless the caller provides it;
# mods_initialize treats NULL (unset) as disabled.

cd "$SCRIPT_DIR" || exit 1
# Rebuild only costume metadata; the Linux worker needs no .NET/graphics bridge.
export MELEE_COSTUME_REGISTRY="${MELEE_COSTUME_REGISTRY:-$SCRIPT_DIR/build/native-game-linux/costumes.tsv}"
python3 -c "import sys; sys.path.insert(0, 'tools/modkit'); from catalog import costume_registry; costume_registry()" || exit 1
exec ./build/native-game-linux/melee "$DOL" "$@"
