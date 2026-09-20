"""Extract and validate the user's Melee 1.02 GameCube disc.
Only local user-provided input is read. Disc assets are excluded from version control.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

DOL_SHA1 = "08e0bf20134dfcb260699671004527b2d6bb1a45"

def be32(data, offset):
    return struct.unpack_from(">I", data, offset)[0]

def read_exact(stream, offset, size):
    stream.seek(offset)
    data = stream.read(size)
    if len(data) != size:
        raise ValueError(f"Truncated disc range {offset:#x}+{size:#x}")
    return data

def archive_info(data):
    if len(data) < 32 or be32(data, 0) != len(data):
        return None
    size, body, reloc, public, extern = struct.unpack_from(">5I", data)
    table_end = 32 + body + 4*reloc + 8*(public + extern)
    if table_end > size:
        raise ValueError("Archive tables exceed file size")
    offsets = [be32(data, 32 + body + 4*i) for i in range(reloc)]
    for off in offsets:
        if off + 4 > body:
            raise ValueError(f"Invalid relocation site {off:#x}")
        target = be32(data, 32 + off)
        if target > body:
            raise ValueError(f"Invalid relocation target {target:#x}")
    symbols = []
    for i in range(public + extern):
        off, name = struct.unpack_from(">2I", data, 32 + body + 4*reloc + 8*i)
        start = table_end + name
        end = data.find(b"\0", start)
        if start >= size or end == -1:
            raise ValueError("Invalid archive symbol string")
        if i < public and off > body:
            raise ValueError("Invalid public data offset")
        symbols.append({"name": data[start:end].decode("ascii"), "offset": off,
                        "external": i >= public})
    return {"body_size": body, "relocations": reloc, "symbols": symbols}

def extract(iso, destination):
    destination = destination.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    result = {"disc": str(iso.resolve()), "files": [], "archives": []}
    with iso.open("rb") as stream:
        header = read_exact(stream, 0, 0x440)
        if header[:6] != b"GALE01" or header[7] != 2 or be32(header, 0x1c) != 0xc2339f3d:
            raise ValueError("Expected a Melee USA 1.02 GameCube ISO")
        dol_off, fst_off, fst_size = struct.unpack_from(">3I", header, 0x420)
        dol_header = read_exact(stream, dol_off, 0x100)
        dol_size = max(be32(dol_header, 4*i) + be32(dol_header, 0x90 + 4*i)
                       for i in range(18))
        dol = read_exact(stream, dol_off, dol_size)
        result["dol_sha1"] = hashlib.sha1(dol).hexdigest()
        if result["dol_sha1"] != DOL_SHA1:
            raise ValueError(f"Unexpected DOL SHA-1: {result['dol_sha1']}")
        (destination / "sys").mkdir(exist_ok=True)
        (destination / "sys/main.dol").write_bytes(dol)
        fst = read_exact(stream, fst_off, fst_size)
        (destination / "sys/fst.bin").write_bytes(fst)
        count = be32(fst, 8)
        if count < 1 or count*12 > len(fst):
            raise ValueError("Invalid disc filesystem table")
        names = fst[count*12:]
        stack = [(count, Path("files"))]
        for i in range(1, count):
            while i >= stack[-1][0]:
                stack.pop()
            flags, off, size = struct.unpack_from(">3I", fst, i*12)
            nameoff = flags & 0xffffff
            end = names.find(b"\0", nameoff)
            if nameoff >= len(names) or end < 0:
                raise ValueError("Invalid disc filename")
            name = names[nameoff:end].decode("ascii")
            if name in ("", ".", "..") or any(c in name for c in '/\\:'):
                raise ValueError("Unsafe disc filename")
            relative = stack[-1][1] / name
            target = (destination / relative).resolve()
            target.relative_to(destination)
            if flags >> 24:
                if size <= i or size > stack[-1][0]:
                    raise ValueError("Invalid disc directory range")
                target.mkdir(parents=True, exist_ok=True)
                stack.append((size, relative))
                continue
            data = read_exact(stream, off, size)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            result["files"].append({"path": relative.as_posix(), "size": size})
            if target.suffix.lower() in (".dat", ".usd"):
                info = archive_info(data)
                if info:
                    info["path"] = relative.as_posix()
                    result["archives"].append(info)
        (destination / "manifest.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    return result

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("iso", type=Path)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1] / "data/GALE01")
    args = parser.parse_args()
    report = extract(args.iso, args.output)
    print(f"DOL SHA-1 verified: {report['dol_sha1']}")
    print(f"Extracted {len(report['files'])} files; validated {len(report['archives'])} HSD archives")
