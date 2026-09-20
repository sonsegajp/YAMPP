"""Pinned upstream build identities. The relay never stores or serves these mods."""
import json
from pathlib import Path

FEATURE = "upstream-builds-v1"
BUILDS = json.loads(Path(__file__).with_suffix(".json").read_text(encoding="utf-8"))["builds"]


def identity(build):
    return {"id": build["id"], "version": build["version"], "sha256": build["archive"]["sha256"]}


def release(value):
    if not isinstance(value, dict) or set(value) != {"id", "version", "sha256"}:
        raise ValueError("Invalid upstream build identity")
    for build in BUILDS:
        if value == identity(build):
            return build
    raise ValueError("This upstream build version is not supported; update YAMPP")


def clean_build(value):
    return None if value is None else identity(release(value))


def download_offer(value):
    build = release(value)
    return {**identity(build), "name": build["name"], "source": "github",
            "repository": build["repository"], "release_page": build["releasePage"],
            "size": build["archive"]["size"] + build["project"]["size"]}
