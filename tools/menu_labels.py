"""Build menu label recipes for the native runtime.

Melee's menu item labels ("1-P Mode", "Options", ...) and the italic panel
headers are textures inside MnMaAll.usd. New labels ("Netplay", "Widescreen",
"Host Room", ...) are composed at runtime from glyph columns of those original
textures, so no derived artwork is stored in the repository. This script
measures the glyph columns from the extracted disc data and writes the recipe
as C tables, plus preview images for inspection.

Usage: python tools/menu_labels.py [--preview-only]
Requires build/modkit/bridge/ModBridge.exe (symbol-textures command).
"""
from pathlib import Path
import argparse, base64, json, struct, subprocess, sys, zlib

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / "build/netplay-work"
ARCHIVE = ROOT / "data/GALE01/files/MnMaAll.usd"
BRIDGE = ROOT / "build/modkit/bridge/ModBridge.exe"
OUT = ROOT / "native/host/gxrt/menu_labels.inc"

ITALIC_SHEAR = 0.30       # panel headers are slanted; segment in sheared space
BOLD_CORE_ALPHA = 200     # white core of the outlined bold font
ITALIC_ALPHA = 160
BOLD_PAD = 1              # columns kept around a bold core for its outline
BOLD_GAP = 1
ITALIC_GAP = 2

# Words in the cursor label table (MenMainCursor_Top matanim, joint 3), by image
# index. Each entry must segment into exactly one column range per character.
BOLD_WORDS = {
    0: "1-PMode", 1: "VS.Mode", 2: "Trophies", 3: "Options", 4: "Data", 8: "Stadium",
    9: "Training", 10: "Melee", 13: "CustomRules", 14: "NameEntry", 15: "Gallery",
    16: "Lottery", 19: "Rumble", 20: "Sound", 21: "ScreenDisplay", 26: "Archives",
    24: "EraseData", 25: "Snapshots", 29: "Special", 30: "Classic", 31: "Adventure",
    32: "All-Star", 35: "TargetTest", 46: "InvisibleMelee", 43: "SuperSuddenDeath",
    54: "BonusRecords", 36: "Home-RunContest",
}
# Words in the panel header table (MenMainPanel_Top matanim, joint 85).
ITALIC_WORDS = {
    1: "EventMatch", 3: "Stadium", 4: "Training", 7: "SpecialMelee", 8: "CustomRules",
    9: "NameEntry", 10: "Rumble", 11: "Sound", 15: "EraseData", 16: "Snapshots",
    17: "Archives", 18: "SoundTest", 20: "Special",
}

# Preferred source for each character: (image index, position in that word).
# A character absent here is looked up in the first word that contains it.
BOLD_PREFERRED = {"o": (20, 1), "e": (10, 1), "a": (4, 1), "M": (10, 0), "S": (20, 0)}
ITALIC_PREFERRED = {"e": (10, 5), "o": (11, 1), "a": (3, 2), "S": (11, 0), "M": (7, 7)}

# Labels to build. Melee's menu font has no W, so it is the M glyph turned 180
# degrees; f, j, k, q, x and z are absent from these words, so label text avoids
# them (numbers and free text are drawn with the game's own SIS font instead).
BOLD_LABELS = {
    "controllers": "Controller", "netplay": "Online", "widescreen": "Widescreen", "host_room": "Host Room",
    "enter_room": "Enter Room", "leave_room": "Leave Room", "ready": "Ready",
    "not_ready": "Not Ready", "start_match": "Start Match", "return": "Return",
    "server": "Server", "name": "Name", "connect": "Connect", "disconnect": "Disconnect",
    "rooms": "Rooms", "lives": "Lives", "time": "Time", "items": "Items",
    "delay": "Delay", "damage": "Damage", "mode": "Mode",
}
ITALIC_LABELS = {"netplay": "Online", "widescreen": "Widescreen", "room": "Room", "rooms": "Rooms"}


def dump(symbol):
    path = WORK / (symbol + ".json")
    if not path.exists():
        WORK.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(BRIDGE), "symbol-textures", str(ARCHIVE), symbol, "8000", str(path)], check=True)
    return json.loads(path.read_text())


def texanim_images(doc, joint):
    for j in doc["joints"]:
        if j["index"] != joint:
            continue
        for m in j["mats"]:
            for ta in m["texanims"]:
                return ta["images"]
    raise SystemExit("texanim not found for joint %d" % joint)


def rgba(image):
    return image["width"], image["height"], base64.b64decode(image["rgba"])


def segments(image, shear, core):
    w, h, d = rgba(image)
    cols = [0] * (w + 80)
    for y in range(h):
        shift = int(round(y * shear))
        for x in range(w):
            r, g, b, a = d[(y * w + x) * 4:(y * w + x) * 4 + 4]
            on = (a > BOLD_CORE_ALPHA and r > 200) if core else (a > ITALIC_ALPHA)
            if on:
                cols[x + shift] += 1
    out, start = [], None
    for x, c in enumerate(cols + [0]):
        if c and start is None:
            start = x
        elif not c and start is not None:
            out.append((start, x))
            start = None
    return out


def atlas(images, words, preferred, shear, core, pad):
    """character -> (image index, u0, u1) taken from the original textures."""
    ranges = {}
    for index, word in words.items():
        segs = segments(images[index], shear, core)
        if len(segs) != len(word):
            raise SystemExit("segmentation mismatch for %r: %d segments" % (word, len(segs)))
        ranges[index] = segs
    table = {}
    for index, word in sorted(words.items()):
        for position, ch in enumerate(word):
            if ch not in table:
                u0, u1 = ranges[index][position]
                table[ch] = (index, u0 - pad, u1 + pad)
    for ch, (index, position) in preferred.items():
        u0, u1 = ranges[index][position]
        table[ch] = (index, u0 - pad, u1 + pad)
    return table


def recipe(table, text, gap, width, space):
    glyphs, total = [], 0
    for ch in text:
        if ch == " ":
            glyphs.append(None)
            total += space
            continue
        source = table.get(ch)
        rotate = 0
        if source is None and ch in ("W", "w"):
            source = table["M"]
            rotate = 1
        if source is None:
            raise SystemExit("no glyph for %r in %r (have: %s)" % (ch, text, "".join(sorted(table))))
        index, u0, u1 = source
        glyphs.append((index, u0, u1, rotate))
        total += (u1 - u0)
    total += gap * (len(text) - 1)
    cursor = (width - total) // 2
    if cursor < 0:
        raise SystemExit("label %r is too wide (%d > %d)" % (text, total, width))
    out = []
    for glyph in glyphs:
        if glyph is None:
            cursor += space + gap
            continue
        index, u0, u1, rotate = glyph
        out.append((index, u0, u1, rotate, cursor))
        cursor += (u1 - u0) + gap
    return out


def compose_preview(images, placements, shear, width, height):
    buf = bytearray(b"\x2a\x2e\x4a\xff" * (width * height))
    for index, u0, u1, rotate, dst in placements:
        w, h, d = rgba(images[index])
        box = [[None] * (u1 - u0) for _ in range(h)]
        for y in range(h):
            shift = int(round(y * shear))
            for u in range(u0, u1):
                x = u - shift
                if 0 <= x < w:
                    box[y][u - u0] = d[(y * w + x) * 4:(y * w + x) * 4 + 4]
        if rotate:
            box = [row[::-1] for row in box[::-1]]
        for y in range(h):
            shift = int(round(y * shear))
            for k in range(u1 - u0):
                px = box[y][k]
                if not px:
                    continue
                r, g, b, a = px
                x = dst + k - shift
                if not (0 <= x < width) or not a:
                    continue
                i = (y * width + x) * 4
                buf[i] = (r * a + buf[i] * (255 - a)) // 255
                buf[i + 1] = (g * a + buf[i + 1] * (255 - a)) // 255
                buf[i + 2] = (b * a + buf[i + 2] * (255 - a)) // 255
    return buf


def png(w, h, data):
    raw = b"".join(b"\0" + bytes(data[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def sheet(entries, path, width, height, columns=3):
    rows = (len(entries) + columns - 1) // columns
    W, H = columns * (width + 6), rows * (height + 6)
    buf = bytearray(b"\x10\x10\x18\xff" * (W * H))
    for n, (name, pixels) in enumerate(entries):
        cx, cy = (n % columns) * (width + 6), (n // columns) * (height + 6)
        for y in range(height):
            for x in range(width):
                s = (y * width + x) * 4
                d = ((cy + y) * W + cx + x) * 4
                buf[d:d + 4] = pixels[s:s + 4]
    path.write_bytes(png(W, H, buf))


def render_sis_image(text, width, height, font_size):
    """Rasterise exact original SIS glyphs and their native proportional metrics."""
    from PIL import Image
    from extract_netplay_fonts import sis_glyphs
    import hashlib
    dol = (ROOT / "data/GALE01/sys/main.dol").read_bytes()
    if hashlib.sha1(dol).hexdigest() != "08e0bf20134dfcb260699671004527b2d6bb1a45":
        raise ValueError("Expected original NTSC 1.02 Melee DOL")
    glyphs, line_height = sis_glyphs(dol)
    table = {code: (glyph, dx, dy, advance) for code, glyph, dx, dy, advance in glyphs}
    advance = sum(table[ord(ch)][3] for ch in text)
    original = Image.new("RGBA", (advance, line_height))
    x = 0
    for ch in text:
        glyph, dx, dy, step = table[ord(ch)]
        original.alpha_composite(glyph, (x + dx, dy))
        x += step
    scale = min(font_size / line_height, (width - 4) / advance, (height - 2) / line_height)
    resized = original.resize((round(advance * scale), round(line_height * scale)), Image.Resampling.LANCZOS)
    image = Image.new("RGBA", (width, height))
    image.alpha_composite(resized, ((width - resized.width) // 2, (height - resized.height) // 2))
    return image


def render_ia4(image):
    width, height = image.size
    output = bytearray(((width + 7) // 8) * ((height + 3) // 4) * 32)
    for y in range(height):
        for x in range(width):
            index = ((y // 4) * ((width + 7) // 8) + x // 8) * 32 + (y % 4) * 8 + x % 8
            r, g, b, a = image.getpixel((x, y))
            output[index] = ((a >> 4) << 4) | (r >> 4)
    return bytes(output)


def render_i4(text, width, height, font_size):
    """Rasterise white text into a GameCube I4 tiled texture (8x8 tiles, 4bpp).

    Used for the two option squares of the widescreen submenu, which are plain
    I4 word images in the original screen.
    """
    image = render_sis_image(text, width, height, font_size).getchannel("A")
    pixels = image.load()
    tiles_w, tiles_h = (width + 7) // 8, (height + 7) // 8
    data = bytearray(tiles_w * tiles_h * 32)
    for ty in range(tiles_h):
        for tx in range(tiles_w):
            for y in range(8):
                for x in range(8):
                    px, py = tx * 8 + x, ty * 8 + y
                    v = pixels[px, py] >> 4 if (px < width and py < height) else 0
                    data[(ty * tiles_w + tx) * 32 + y * 4 + x // 2] |= v << (4 if x % 2 == 0 else 0)
    return bytes(data), image


def c_array(name, data):
    lines = ["static const unsigned char %s[%d] = {" % (name, len(data))]
    for i in range(0, len(data), 24):
        lines.append("    " + ",".join("0x%02x" % b for b in data[i:i + 24]) + ",")
    lines.append("};")
    return chr(10).join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--preview-only", action="store_true")
    args = parser.parse_args()
    cursor = texanim_images(dump("MenMainCursor_Top_matanim_joint"), 3)
    panel_doc = dump("MenMainPanel_Top_matanim_joint")
    panel = list(texanim_images(panel_doc, 85))
    parent_start = len(panel)
    panel.append(texanim_images(panel_doc, 84)[3])  # original Options supplies uppercase O
    italic_words = dict(ITALIC_WORDS)
    italic_words[parent_start] = "Options"
    bold = atlas(cursor, BOLD_WORDS, BOLD_PREFERRED, 0.0, True, BOLD_PAD)
    italic = atlas(panel, italic_words, ITALIC_PREFERRED, ITALIC_SHEAR, False, 0)
    print("bold glyphs:  ", "".join(sorted(bold)))
    print("italic glyphs:", "".join(sorted(italic)))

    lines = ["/* Generated by tools/menu_labels.py from the extracted MnMaAll.usd; do not edit. */",
             "typedef struct { unsigned char table, src, rot; short u0, u1, dst; } LabelGlyph;",
             "/* One entry per character the disc's own label textures can supply, so that",
             "   text only known at runtime - room names - can be composed the same way. */",
             "typedef struct { char ch; unsigned char table, src, rot; short u0, u1; } LabelChar;",
             "typedef struct { const char* name; const LabelGlyph* glyphs; unsigned count; } LabelRecipe;",
             "#define LABEL_TABLE_BOLD 0",
             "#define LABEL_TABLE_ITALIC 1",
             "#define LABEL_TABLE_PARENT_ITALIC 2",
             "#define LABEL_ITALIC_SHEAR %.2ff" % ITALIC_SHEAR]
    previews = {"bold": [], "italic": []}
    for kind, table, labels, images, shear, gap, width, height, space in (
            ("bold", bold, BOLD_LABELS, cursor, 0.0, BOLD_GAP, 176, 30, 7),
            ("italic", italic, ITALIC_LABELS, panel, ITALIC_SHEAR, ITALIC_GAP, 168, 28, 7)):
        table_id = "LABEL_TABLE_BOLD" if kind == "bold" else "LABEL_TABLE_ITALIC"
        for key, text in labels.items():
            placements = recipe(table, text, gap, width, space)
            previews[kind].append((key, compose_preview(images, placements, shear, width, height)))
            lines.append("static const LabelGlyph label_%s_%s[] = {" % (kind, key))
            for index, u0, u1, rotate, dst in placements:
                source_table = "LABEL_TABLE_PARENT_ITALIC" if kind == "italic" and index == parent_start else table_id
                source_index = 3 if source_table == "LABEL_TABLE_PARENT_ITALIC" else index
                lines.append("    {%s,%d,%d,%d,%d,%d}," % (source_table, source_index, rotate, u0, u1, dst))
            lines.append("};")
        lines.append("static const LabelRecipe label_%s_recipes[] = {" % kind)
        for key in labels:
            lines.append('    {"%s", label_%s_%s, sizeof label_%s_%s / sizeof(LabelGlyph)},' % (key, kind, key, kind, key))
        lines.append("};")
    WORK.mkdir(parents=True, exist_ok=True)
    for kind, table, gap, space in (("bold", bold, BOLD_GAP, 7), ("italic", italic, ITALIC_GAP, 7)):
        table_id = "LABEL_TABLE_BOLD" if kind == "bold" else "LABEL_TABLE_ITALIC"
        lines.append("static const LabelChar label_%s_chars[] = {" % kind)
        for ch in sorted(table):
            index, u0, u1 = table[ch]
            source_table = "LABEL_TABLE_PARENT_ITALIC" if kind == "italic" and index == parent_start else table_id
            source_index = 3 if source_table == "LABEL_TABLE_PARENT_ITALIC" else index
            lines.append("    {'%s',%s,%d,0,%d,%d}," % (ch, source_table, source_index, u0, u1))
        # W has no glyph of its own in the menu textures; it is an upside-down M.
        for ch in ("W", "w"):
            if ch not in table and "M" in table:
                index, u0, u1 = table["M"]
                lines.append("    {'%s',%s,%d,1,%d,%d}," % (ch, table_id, index, u0, u1))
        lines.append("};")
        lines.append("#define LABEL_%s_GAP %d" % (kind.upper(), gap))
        lines.append("#define LABEL_%s_SPACE %d" % (kind.upper(), space))
    sheet(previews["bold"], WORK / "labels_bold.png", 176, 30)
    sheet(previews["italic"], WORK / "labels_italic.png", 168, 28)
    narrow, narrow_png = render_i4("4:3", 80, 24, 19)
    wide, wide_png = render_i4("16:9", 104, 25, 20)
    narrow_png.save(WORK / "label_4_3.png")
    wide_png.save(WORK / "label_16_9.png")
    lines += ["#define LABEL_NARROW_W 80", "#define LABEL_NARROW_H 24", "#define LABEL_WIDE_W 104", "#define LABEL_WIDE_H 25"]
    lines.append(c_array("label_narrow_i4", narrow))
    lines.append(c_array("label_wide_i4", wide))
    mod_browser = render_sis_image("Mod Browser", 176, 30, 29)
    mod_browser.save(WORK / "label_mod_browser.png")
    lines.append(c_array("label_mod_browser_ia4", render_ia4(mod_browser)))
    profile = render_sis_image("Profile", 176, 30, 29)
    profile.save(WORK / "label_profile.png")
    lines.append(c_array("label_profile_ia4", render_ia4(profile)))
    if args.preview_only:
        print("previews written to", WORK)
        return
    OUT.write_text("\n".join(lines) + "\n")
    print("wrote", OUT, "with", len(BOLD_LABELS), "bold and", len(ITALIC_LABELS), "italic labels")


if __name__ == "__main__":
    main()
