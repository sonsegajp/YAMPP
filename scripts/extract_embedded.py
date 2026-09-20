"""Generate Melee's embedded font headers from the hash-verified original DOL."""
import hashlib
import re
import struct
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
dol = (ROOT / "data/GALE01/sys/main.dol").read_bytes()
if hashlib.sha1(dol).hexdigest() != "08e0bf20134dfcb260699671004527b2d6bb1a45":
    raise SystemExit("Unexpected Melee DOL")
symbols = (ROOT / "upstream/melee/config/GALE01/symbols.txt").read_text()
for symbol, filename, record_size in [
    ("HSD_DebugFontAtlas", "debug_font.inc", 56),
    ("HSD_SisLib_FontAtlas", "sislib_font.inc", 512),
]:
    found = re.search(re.escape(symbol) + r" = \.data:(0x[0-9A-Fa-f]+);.*size:(0x[0-9A-Fa-f]+)", symbols)
    if not found:
        raise ValueError("Missing symbol " + symbol)
    address, size = (int(s, 16) for s in found.groups())
    payload = None
    for i in range(18):
        offset = struct.unpack_from(">I", dol, i*4)[0]
        start = struct.unpack_from(">I", dol, 0x48+i*4)[0]
        length = struct.unpack_from(">I", dol, 0x90+i*4)[0]
        if start <= address and address+size <= start+length:
            payload = dol[offset+address-start:offset+address-start+size]
            break
    if payload is None or len(payload) != size or size % record_size:
        raise ValueError("Invalid embedded font range")
    output = ROOT / "build/generated/sysdolphin/baselib" / filename
    output.parent.mkdir(parents=True, exist_ok=True)
    records = []
    for off in range(0, size, record_size):
        records.append("{{" + ",".join(f"0x{b:02x}" for b in payload[off:off+record_size]) + "}},")
    output.write_text("\n".join(records) + "\n")
    print(f"{symbol}: {size} bytes, {size//record_size} records")
