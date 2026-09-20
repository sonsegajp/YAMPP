"""Build netplay bitmap fonts from the original, verified Melee disc pixels.

The SIS atlas and proportional metrics come directly from NTSC 1.02 main.dol.
Menu bold and panel italic glyphs are segmented from MnMaAll.usd label textures
using the existing menu_labels.py recipe authority. No system fonts, invented
glyphs, rotation, or geometric font approximations are used.
"""
from __future__ import annotations

import ast
import hashlib
from io import BytesIO
import json
from pathlib import Path
import re
import struct

from PIL import Image, ImageDraw
import menu_labels

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "native/host/aurora_shim/netplay_fonts.inc"
ART = ROOT / "build/art/native-fonts"


def dol_bytes(dol: bytes, address: int, size: int) -> bytes:
    for i in range(18):
        offset = struct.unpack_from(">I", dol, i * 4)[0]
        start = struct.unpack_from(">I", dol, 0x48 + i * 4)[0]
        length = struct.unpack_from(">I", dol, 0x90 + i * 4)[0]
        if start <= address and address + size <= start + length:
            return dol[offset + address - start:offset + address - start + size]
    raise ValueError(f"Missing DOL range {address:08X}+{size:X}")


def sis_glyphs(dol: bytes):
    source = ROOT / "upstream/HSDLib/HSDRaw/Tools/Melee/MeleeMenuText.cs"
    mapping = source.read_text(encoding="utf-8").split("public static List<char> CharMAP", 1)[1]
    chars = [ast.literal_eval("'" + item + "'")
             for item in re.findall(r"'((?:\\.|[^'])*)'", mapping)]
    if len(chars) != 287:
        raise ValueError("Unexpected original SIS character mapping")
    tiles = dol_bytes(dol, 0x8040CD40, 287 * 512)
    metrics = dol_bytes(dol, 0x8040CB00, 288 * 2)
    result = [(32, Image.new("RGBA", (1, 1)), 0, 0, 16)]
    for index, char in enumerate(chars):
        image = Image.new("RGBA", (32, 32))
        pixels = image.load()
        for y in range(32):
            for x in range(32):
                tile = (y // 8) * 4 + x // 8
                packed = tiles[index * 512 + tile * 32 + (y % 8) * 4 + (x % 8) // 2]
                alpha = ((packed >> 4) if x % 2 == 0 else packed & 15) * 17
                pixels[x, y] = (255, 255, 255, alpha)
        left, right = metrics[index * 2:index * 2 + 2]
        # HSD_SisLib_803A8134 / 803A84BC: 32px cell with proportional
        # advance 32-(left+right-2) and draw origin x-(left-1).
        result.append((ord(char), image, 1 - left, 0, 34 - left - right))
    return result, 32


def menu_glyphs(italic: bool):
    m = menu_labels
    symbol = "MenMainPanel_Top_matanim_joint" if italic else "MenMainCursor_Top_matanim_joint"
    doc = m.dump(symbol)
    images = list(m.texanim_images(doc, 85 if italic else 3))
    words = dict(m.ITALIC_WORDS if italic else m.BOLD_WORDS)
    if italic:
        # Parent-menu headings have the authentic uppercase O used by Options.
        parent = m.texanim_images(doc, 84)
        words[len(images)] = "Options"
        images.append(parent[3])
    shear, pad = (m.ITALIC_SHEAR, 0) if italic else (0, m.BOLD_PAD)
    table = m.atlas(images, words,
                    m.ITALIC_PREFERRED if italic else m.BOLD_PREFERRED,
                    shear, not italic, pad)
    height = images[next(iter(table.values()))[0]]["height"]
    shift_pad = round((height - 1) * shear)
    result = [(32, Image.new("RGBA", (1, 1)), 0, 0, 8)]
    for char, (index, u0, u1) in sorted(table.items()):
        width, h, data = m.rgba(images[index])
        source = Image.frombytes("RGBA", (width, h), data)
        patch = Image.new("RGBA", (u1 - u0 + shift_pad, height))
        pixels, source_pixels = patch.load(), source.load()
        for y in range(height):
            shift = round(y * shear)
            for u in range(u0, u1):
                sx = u - shift
                if 0 <= sx < width:
                    pixels[u - u0 - shift + shift_pad, y] = source_pixels[sx, y]
        result.append((ord(char), patch, -shift_pad, 0,
                       u1 - u0 + (m.ITALIC_GAP if italic else m.BOLD_GAP)))
    return result, height


def pack_font(name, glyphs, line_height, lines):
    cell_w = max(g[1].width for g in glyphs) + 4
    cell_h = max(g[1].height for g in glyphs) + 4
    columns = 16
    atlas = Image.new("RGBA", (columns * cell_w, ((len(glyphs) + columns - 1) // columns) * cell_h))
    records = []
    for index, (code, glyph, dx, dy, advance) in enumerate(glyphs):
        x, y = (index % columns) * cell_w + 2, (index // columns) * cell_h + 2
        atlas.paste(glyph, (x, y))
        records.append((code, x, y, glyph.width, glyph.height, dx, dy, advance))
    stream = BytesIO()
    atlas.save(stream, format="PNG", optimize=True)
    encoded = stream.getvalue()
    (ART / (name + ".png")).write_bytes(encoded)
    lines.append(f"static const unsigned char {name}_png[] = {{")
    lines.extend(",".join(f"0x{value:02x}" for value in encoded[i:i + 24]) + ","
                 for i in range(0, len(encoded), 24))
    lines.append("};")
    lines.append(f"static const netplay_art::Image {name}_image = "
                 f"{{ {name}_png, sizeof({name}_png), {atlas.width}, {atlas.height} }};")
    lines.append(f"static const Glyph {name}_glyphs[] = {{")
    lines.extend("{" + ",".join(str(value) for value in record) + "}," for record in sorted(records))
    lines.append("};")
    lines.append(f"static const Font {name} = {{ &{name}_image, {name}_glyphs, "
                 f"sizeof({name}_glyphs)/sizeof(Glyph), {line_height} }};")
    return {"glyphs": len(glyphs), "line_height": line_height, "png_bytes": len(encoded),
            "sha256": hashlib.sha256(encoded).hexdigest(), "metrics": records}


def preview(fonts):
    image = Image.new("RGBA", (900, 300), (18, 23, 35, 255))
    examples = [("italic", "Online  Rooms"),
                ("bold", "Start Match   Change Rules   Leave Room"),
                ("sis", "Room Browser  UI Tester  4 Stock  8 Min"),
                ("sis", "A Confirm   X Unready   B Back"),
                ("sis", "0123456789  127.0.0.1:7777  !?/%_+-")]
    for row, (name, text) in enumerate(examples):
        glyphs, height = fonts[name]
        table = {code: (glyph, dx, dy, advance) for code, glyph, dx, dy, advance in glyphs}
        x = 24
        for ch in text:
            glyph, dx, dy, advance = table[ord(ch)]
            image.alpha_composite(glyph, (x + dx, 14 + row * 55 + dy))
            x += advance
    image.save(ART / "native-font-preview.png")


def main():
    dol_path = ROOT / "data/GALE01/sys/main.dol"
    dol = dol_path.read_bytes()
    if hashlib.sha1(dol).hexdigest() != "08e0bf20134dfcb260699671004527b2d6bb1a45":
        raise ValueError("Expected original NTSC 1.02 Melee DOL")
    ART.mkdir(parents=True, exist_ok=True)
    fonts = {"sis": sis_glyphs(dol), "bold": menu_glyphs(False), "italic": menu_glyphs(True)}
    # Native bold has a white core and a black outline in one IA4 image.
    # Dark selected labels use the original core coverage, not a blackened
    # outline silhouette; glyph pixels/metrics remain derived from the disc.
    cores = []
    for code, glyph, dx, dy, advance in fonts["bold"][0]:
        core = glyph.copy()
        core.putdata([(255, 255, 255, a * max(r, g, b) // 255)
                      for r, g, b, a in glyph.getdata()])
        cores.append((code, core, dx, dy, advance))
    fonts["bold_core"] = (cores, fonts["bold"][1])
    lines = ["/* Generated by tools/extract_netplay_fonts.py from original Melee pixels. */",
             "namespace netplay_font {",
             "struct Glyph { unsigned code; unsigned short x,y,w,h; short dx,dy,advance; };",
             "struct Font { const netplay_art::Image* image; const Glyph* glyphs; unsigned count, height; };"]
    report = {"dol_sha1": hashlib.sha1(dol).hexdigest(),
              "sis_atlas_address": "0x8040CD40", "sis_metrics_address": "0x8040CB00",
              "menu_archive": "data/GALE01/files/MnMaAll.usd",
              "menu_archive_sha256": hashlib.sha256(menu_labels.ARCHIVE.read_bytes()).hexdigest(),
              "system_fonts": False, "synthesized_glyphs": False, "fonts": {}}
    for name, (glyphs, height) in fonts.items():
        report["fonts"][name] = pack_font(name, glyphs, height, lines)
    lines.append("} // namespace netplay_font")
    OUT.write_text("\n".join(lines) + "\n", encoding="ascii", newline="\n")
    (ART / "manifest.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    preview(fonts)
    print(f"Extracted original SIS, menu bold, and panel italic fonts to {OUT}")
    print(f"Review atlas PNGs and extraction manifest in {ART}")


if __name__ == "__main__":
    main()
