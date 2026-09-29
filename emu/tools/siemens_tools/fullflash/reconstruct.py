from __future__ import annotations

import argparse
import hashlib
import json
import os
import tempfile
from pathlib import Path
from typing import Any

from .normalization import restore_normalization
from .catalog_backing import validate_official_backing
from ..firmware.xbi import FirmwareError


SCHEMA = "siemens-fullflash-recipe"
SCHEMA_VERSION = 4


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _load_recipe(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise FirmwareError(f"invalid fullflash recipe {path}: {exc}") from exc
    if (
        not isinstance(document, dict)
        or document.get("schema") != SCHEMA
        or document.get("schema_version") != SCHEMA_VERSION
    ):
        raise FirmwareError(f"unsupported fullflash recipe schema: {path}")
    if not isinstance(document.get("fullflash"), dict):
        raise FirmwareError("fullflash recipe has no fullflash object")
    if not isinstance(document.get("slices"), list):
        raise FirmwareError("fullflash recipe has no slices array")
    return document


def _payload_path(
    corpus_root: Path, value: object, external_paths: set[str],
) -> Path:
    if not isinstance(value, str):
        raise FirmwareError("recipe slice has no payload_path")
    relative = Path(value)
    if (
        relative.is_absolute()
        or not relative.parts
        or any(part in ("", ".", "..") for part in relative.parts)
    ):
        raise FirmwareError(f"unsafe recipe payload path: {value!r}")
    resolved_root = corpus_root.resolve()
    resolved = (corpus_root / relative).resolve()
    try:
        resolved.relative_to(resolved_root)
    except ValueError as exc:
        if value not in external_paths:
            raise FirmwareError(
                f"recipe payload escapes corpus root: {value!r}"
            ) from exc
    return resolved


def reconstruct_recipe(path: Path) -> bytes:
    """Validate and reconstruct one content-addressed fullflash recipe."""
    document = _load_recipe(path)
    corpus_root = path.parent.parent
    external_paths: set[str] = set()
    catalog_path = corpus_root / "catalog.json"
    if catalog_path.is_file():
        try:
            catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as exc:
            raise FirmwareError(
                f"invalid fullflash catalog {catalog_path}: {exc}"
            ) from exc
        if isinstance(catalog, dict):
            validate_official_backing(catalog, catalog_path)
            external_paths = {
                record["path"]
                for region in catalog.get("regions", [])
                for variant in region.get("variants", [])
                for record in (variant.get("official_backing") or {}).get(
                    "paths", []
                )
            }
    fullflash = document["fullflash"]
    expected_size = fullflash.get("size")
    expected_sha = fullflash.get("sha256")
    if (
        not isinstance(expected_size, int)
        or expected_size <= 0
        or not isinstance(expected_sha, str)
        or len(expected_sha) != 64
    ):
        raise FirmwareError("fullflash recipe has invalid output identity")

    rebuilt = bytearray()
    for expected_order, item in enumerate(document["slices"]):
        if not isinstance(item, dict) or item.get("order") != expected_order:
            raise FirmwareError("recipe slices are not in contiguous order")
        source_range = item.get("source_range")
        if not isinstance(source_range, dict):
            raise FirmwareError("recipe slice has no source range")
        start = source_range.get("from")
        end = source_range.get("to_exclusive")
        length = source_range.get("length")
        size = item.get("size")
        digest = item.get("sha256")
        if (
            not isinstance(start, int)
            or not isinstance(end, int)
            or not isinstance(length, int)
            or not isinstance(size, int)
            or start != len(rebuilt)
            or end < start
            or length != end - start
            or size != length
            or not isinstance(digest, str)
            or len(digest) != 64
        ):
            raise FirmwareError("recipe slice range, size, or hash is invalid")
        has_payload_path = "payload_path" in item
        has_erased = "erased" in item
        if has_payload_path == has_erased:
            raise FirmwareError(
                "recipe slice must have exactly one storage form"
            )

        if has_erased:
            if item["erased"] is not True:
                raise FirmwareError("recipe slice erased marker must be true")
            if "normalization" in item:
                raise FirmwareError(
                    "erased recipe slice has unexpected normalization"
                )
            payload = b"\xFF" * size
            if _sha256(payload) != digest:
                raise FirmwareError("erased recipe slice hash mismatch")
        else:
            payload_path = _payload_path(
                corpus_root, item.get("payload_path"), external_paths
            )
            if not payload_path.is_file():
                raise FirmwareError(f"recipe payload is missing: {payload_path}")
            payload = payload_path.read_bytes()
            if len(payload) != size:
                raise FirmwareError(
                    f"recipe payload has wrong size: {payload_path}"
                )
            if _sha256(payload) != digest:
                raise FirmwareError(
                    f"recipe payload hash mismatch: {payload_path}"
                )
        normalization = item.get("normalization")
        if normalization is not None:
            payload = restore_normalization(
                payload,
                normalization,
                context=f"recipe slice {expected_order}",
            )
        source_sha256 = item.get("source_sha256")
        if (
            not isinstance(source_sha256, str)
            or len(source_sha256) != 64
            or _sha256(payload) != source_sha256
        ):
            raise FirmwareError("recipe slice source hash mismatch")
        rebuilt.extend(payload)

    result = bytes(rebuilt)
    if len(result) != expected_size:
        raise FirmwareError(
            f"recipe reconstructed {len(result)} bytes, expected {expected_size}"
        )
    actual_sha = _sha256(result)
    if actual_sha != expected_sha:
        raise FirmwareError(
            f"recipe reconstructed SHA-256 {actual_sha}, expected {expected_sha}"
        )
    return result


def _atomic_write(path: Path, data: bytes, force: bool) -> None:
    if path.exists() and not force:
        raise FirmwareError(f"output already exists: {path} (use --force)")
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".part", dir=path.parent
    )
    try:
        with os.fdopen(fd, "wb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


def command_reconstruct(args: argparse.Namespace) -> None:
    result = reconstruct_recipe(args.recipe)
    _atomic_write(args.output, result, args.force)
    print(
        f"written: {args.output} "
        f"({len(result)} bytes, sha256={_sha256(result)})"
    )
