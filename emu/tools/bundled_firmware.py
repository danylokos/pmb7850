"""Manifest-based selection shared by tests, native fixtures, and helpers."""
from __future__ import annotations

import argparse
import hashlib
import json
from functools import lru_cache
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "firmware/manifest.json"


@lru_cache(maxsize=1)
def manifest() -> dict:
    return json.loads(MANIFEST.read_text())


def entries(model: str | None = None) -> list[dict]:
    devices = manifest()["devices"]
    selected = [devices[model.lower()]] if model else devices.values()
    return [entry for default in selected
            for entry in (default, *default.get("alternates", []))]


def entry(model: str, *, software: int | None = None,
          langpack: int | None = None) -> dict:
    for item in entries(model):
        if (software is None or item["software_version"] == software) and (
                langpack is None or item["langpack"] == langpack):
            return item
    raise ValueError(f"No bundled {model} image for SW={software}, LG={langpack}")


@lru_cache(maxsize=None)
def image(model: str, *, software: int | None = None,
          langpack: int | None = None) -> Path:
    item = entry(model, software=software, langpack=langpack)
    path = ROOT / item["image"]
    verify(item)
    return path


def verify(item: dict) -> None:
    path = ROOT / item["image"]
    if not path.is_file():
        raise FileNotFoundError(f"Required bundled firmware missing: {path}")
    data = path.read_bytes()
    if len(data) != item["size"] or hashlib.sha256(data).hexdigest() != item["sha256"]:
        raise ValueError(f"Bundled firmware size/hash mismatch: {path}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--header", type=Path)
    parser.add_argument("--model")
    args = parser.parse_args()
    if args.model:
        print(image(args.model))
    elif args.header:
        lines = ["/* Generated from firmware/manifest.json; do not edit. */",
                 "#ifndef BUNDLED_FIRMWARE_H", "#define BUNDLED_FIRMWARE_H"]
        for model in manifest()["devices"]:
            path = image(model)
            lines.append(f'#define BUNDLED_{model.upper()} {json.dumps(str(path))}')
        lines.append("#endif")
        args.header.parent.mkdir(parents=True, exist_ok=True)
        args.header.write_text("\n".join(lines) + "\n")
    else:
        for item in entries():
            verify(item)
        print(f"Verified {len(entries())} bundled firmware images")


if __name__ == "__main__":
    main()
