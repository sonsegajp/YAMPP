"""Hash the exact native runtime and base-game inputs held read-only by the host.

No persistent hash cache is trusted. The native process obtains a plan, holds
Windows handles denying writes/deletes for every input, then asks this helper
to hash those files. Its in-memory result remains valid for that process only.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from project_config import configuration, disc_path, project_path

RUNTIME_IDENTITY_SCHEMA = 1
RUNTIME_ASPECT_DOMAIN = b"YAMPP-runtime-camera-aspect-v1\0"


def plan(runtime):
    cfg = configuration()
    assets = project_path(cfg["assets"]["directory"])
    runtime = Path(runtime).resolve(strict=True)
    dol = Path(os.environ.get("MELEE_RUNTIME_DOL", str(assets / "sys/main.dol"))).resolve(strict=True)
    fst = Path(os.environ.get("MELEE_FST", str(assets / "sys/fst.bin"))).resolve(strict=True)
    disc = Path(os.environ.get("MELEE_DISC") or disc_path(cfg)).resolve(strict=True)
    records = [{"key": "runtime", "path": str(runtime)},
               {"key": "sys/main.dol", "path": str(dol)},
               {"key": "sys/fst.bin", "path": str(fst)},
               {"key": "disc-image", "path": str(disc)}]
    if os.environ.get("MELEE_RUNTIME_CORE"):
        records.append({"key":"runtime/native-core","path":str(Path(os.environ["MELEE_RUNTIME_CORE"]).resolve(strict=True))})
    base_dol_key = "sys/main.dol"
    if os.environ.get("MELEE_MEX_BASE_DOL"):
        base_dol_key = "baseline/main.dol"
        records.extend([{"key": base_dol_key, "path": str(Path(os.environ["MELEE_MEX_BASE_DOL"]).resolve(strict=True))},
                        {"key": "runtime/mex-engine", "path": str(Path(os.environ["MELEE_UNICORN_LIBRARY"]).resolve(strict=True))}])
    for path in sorted((assets / "files").rglob("*"), key=lambda p: p.relative_to(assets).as_posix()):
        if path.is_file():
            records.append({"key": path.relative_to(assets).as_posix(), "path": str(path.resolve(strict=True))})
    if len(records) < 5 or len(records) > 8192:
        raise ValueError("Base-game assets are incomplete or exceed the supported file count")
    for record in records:
        if not Path(record["path"]).is_file():
            raise ValueError("A required game input is missing")
    return {"schema": 1, "files": sorted(records, key=lambda r: r["key"]),
            "base_dol_key": base_dol_key, "expected_dol_sha1": cfg["identity"]["dolSha1"]}


def decoded_disc_identity(path, decoder=None):
    """Hash the logical disc, not its compression container; no temporary ISO."""
    import subprocess
    decoder = Path(decoder) if decoder else ROOT / "bin" / ("YAMPP-disc-identity.exe" if os.name == "nt" else "YAMPP-disc-identity")
    if not decoder.is_file():
        decoder = ROOT / "build/native-game" / decoder.name
    if not decoder.is_file():
        raise ValueError("Install the ISO/RVZ Online hotfix before joining")
    options = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}
    result = subprocess.run([str(decoder), str(path)], capture_output=True, text=True, timeout=300, **options)
    if result.returncode:
        raise ValueError("Compressed disc verification failed: " + result.stderr.strip()[:240])
    receipt = json.loads(result.stdout)
    digest, size = receipt.get("sha256"), receipt.get("size")
    if (receipt.get("schema") != 1 or not isinstance(digest, str) or len(digest) != 64
            or any(c not in "0123456789abcdef" for c in digest)
            or type(size) is not int or not 0 < size <= 8 * 1024**3):
        raise ValueError("Invalid decoded disc identity")
    return size, bytes.fromhex(digest)


def fingerprint(doc):
    if doc.get("schema") != 1 or not isinstance(doc.get("files"), list):
        raise ValueError("Unsupported compatibility plan")
    records = sorted(doc["files"], key=lambda record: record["key"])
    if len({record["key"] for record in records}) != len(records):
        raise ValueError("Duplicate compatibility input key")
    game = hashlib.sha256(b"MeleePC-base-inputs-v1\0")
    runtime_hash, total = None, 0
    checked_baseline = False
    for record in records:
        path = Path(record["path"])
        digest = hashlib.sha256()
        dol_digest = hashlib.sha1() if record["key"] == doc.get("base_dol_key", "sys/main.dol") else None
        with path.open("rb") as stream:
            before = os.fstat(stream.fileno())
            compressed_disc = record["key"] == "disc-image" and stream.read(4) in (b"RVZ\x01", b"WIA\x01")
            stream.seek(0)
            size = 0
            while True:
                if compressed_disc: break
                block = stream.read(1024 * 1024)
                if not block:
                    break
                digest.update(block)
                if dol_digest:
                    dol_digest.update(block)
                size += len(block)
            logical = decoded_disc_identity(path) if compressed_disc else (size, digest.digest())
            if compressed_disc: size = before.st_size
            after = os.fstat(stream.fileno())
        if size != before.st_size or before.st_size != after.st_size or before.st_mtime_ns != after.st_mtime_ns:
            raise ValueError("A game input changed while it was being verified")
        if dol_digest and dol_digest.hexdigest() != doc["expected_dol_sha1"]:
            raise ValueError("The base game executable does not match the pinned Melee revision")
        if dol_digest: checked_baseline = True
        total += size
        if record["key"] == "runtime":
            runtime_hash = digest.hexdigest()
        else:
            key = record["key"].encode("utf-8")
            game.update(struct.pack(">I", len(key)))
            game.update(key)
            game.update(struct.pack(">Q", logical[0]))
            game.update(logical[1])
    if not checked_baseline: raise ValueError("Pinned base executable is missing from the verification plan")
    if runtime_hash is None:
        raise ValueError("Native runtime was not verified")
    game_hash = game.hexdigest()
    combined = hashlib.sha256(b"MeleePC-compatibility-v1\0" + bytes.fromhex(runtime_hash) + bytes.fromhex(game_hash)).hexdigest()
    # Camera bounds currently depend on aspect, so room compatibility binds it.
    # Both identities reuse the same verified bytes; reconnecting after an
    # offline display change never trusts a persistent file-hash cache.
    runtime_identities = [hashlib.sha256(RUNTIME_ASPECT_DOMAIN + bytes.fromhex(runtime_hash) + bytes([wide])).hexdigest()
                          for wide in (0, 1)]
    aspect_hashes = [hashlib.sha256(b"MeleePC-compatibility-v1\0" + bytes.fromhex(identity) + bytes.fromhex(game_hash)).hexdigest()
                     for identity in runtime_identities]
    return {"schema": 1, "fingerprint": combined, "runtime": runtime_hash,
            "runtimeFileSha256": runtime_hash, "runtimeIdentitySchema": RUNTIME_IDENTITY_SCHEMA,
            "runtimeIdentity4x3": runtime_identities[0], "runtimeIdentity16x9": runtime_identities[1],
            "aspect4x3": aspect_hashes[0], "aspect16x9": aspect_hashes[1],
            "game": game_hash, "files": len(records), "bytes": total}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--plan", action="store_true")
    parser.add_argument("--input-plan", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    code = 0
    try:
        if args.plan:
            result = plan(args.runtime)
        else:
            doc = json.loads(args.input_plan.read_text(encoding="utf-8")) if args.input_plan else plan(args.runtime)
            result = fingerprint(doc)
    except Exception as exc:
        result = {"error": str(exc)}
        code = 1
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(args.output.suffix + ".tmp")
    temporary.write_text(json.dumps(result, ensure_ascii=False), encoding="utf-8")
    temporary.replace(args.output)
    if code:
        print("Game input verification failed", file=sys.stderr)
    raise SystemExit(code)


if __name__ == "__main__":
    main()
