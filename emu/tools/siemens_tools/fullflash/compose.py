from __future__ import annotations

import argparse
import hashlib
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .normalization import restore_normalization
from ..layout.catalog import LoadedLayout, load_layout
from .official_corpus import RECIPE_SCHEMA as OFFICIAL_RECIPE_SCHEMA
from .official_corpus import RECIPE_SCHEMA_VERSION as OFFICIAL_RECIPE_VERSION
from .reconstruct import _atomic_write, reconstruct_recipe
from ..layout.partitions import partition_layout
from ..firmware.xbi import FirmwareError


SCHEMA = "siemens-fullflash-composition"
SCHEMA_VERSION = 1


@dataclass(frozen=True)
class CompositionResult:
    image: bytes
    sha256: str
    overlaps: tuple[dict[str, Any], ...]
    operations: int


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _load_json(path: Path, what: str) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise FirmwareError(f"invalid {what} {path}: {exc}") from exc
    if not isinstance(document, dict):
        raise FirmwareError(f"{what} is not a JSON object: {path}")
    return document


def _relative_path(root: Path, value: object, what: str) -> Path:
    if not isinstance(value, str):
        raise FirmwareError(f"{what} has no path")
    relative = Path(value)
    if (
        relative.is_absolute()
        or not relative.parts
        or any(part in ("", ".", "..") for part in relative.parts)
    ):
        raise FirmwareError(f"unsafe {what} path: {value!r}")
    resolved_root = root.resolve()
    resolved = (root / relative).resolve()
    try:
        resolved.relative_to(resolved_root)
    except ValueError as exc:
        raise FirmwareError(f"{what} path escapes manifest root: {value!r}") from exc
    return resolved


def _validated_range(
    value: object, layout: LoadedLayout, role: object = None,
) -> tuple[int, int]:
    if role is not None:
        if not isinstance(role, str):
            raise FirmwareError("composition role is invalid")
        matches = [
            part for part in partition_layout(layout.layout)
            if part.label == role
        ]
        if len(matches) != 1:
            raise FirmwareError(f"composition role is not unique: {role}")
        return matches[0].start, matches[0].end
    if not isinstance(value, dict):
        raise FirmwareError("composition operation has no range or role")
    start = value.get("from")
    end = value.get("to_exclusive")
    length = value.get("length", end - start if isinstance(start, int)
                       and isinstance(end, int) else None)
    if (
        not isinstance(start, int)
        or not isinstance(end, int)
        or not isinstance(length, int)
        or start < 0
        or end <= start
        or end > layout.layout.length
        or length != end - start
    ):
        raise FirmwareError("composition operation range is invalid")
    return start, end


def _official_operations(recipe_path: Path) -> list[tuple[int, int, bytes, str]]:
    recipe = _load_json(recipe_path, "official package recipe")
    if (
        recipe.get("schema") != OFFICIAL_RECIPE_SCHEMA
        or recipe.get("schema_version") != OFFICIAL_RECIPE_VERSION
        or not isinstance(recipe.get("operations"), list)
    ):
        raise FirmwareError(f"unsupported official package recipe: {recipe_path}")
    corpus_root = recipe_path.parent.parent
    result: list[tuple[int, int, bytes, str]] = []
    for expected_order, operation in enumerate(recipe["operations"]):
        if (
            not isinstance(operation, dict)
            or operation.get("order") != expected_order
        ):
            raise FirmwareError("official recipe operations are not contiguous")
        range_value = operation.get("range")
        if not isinstance(range_value, dict):
            raise FirmwareError("official recipe operation has no range")
        start = range_value.get("from")
        end = range_value.get("to_exclusive")
        size = operation.get("size")
        digest = operation.get("sha256")
        if (
            not isinstance(start, int)
            or not isinstance(end, int)
            or not isinstance(size, int)
            or end - start != size
            or size <= 0
            or not isinstance(digest, str)
            or len(digest) != 64
        ):
            raise FirmwareError("official recipe operation is invalid")
        if operation.get("erased") is True:
            data = b"\xFF" * size
        else:
            path = _relative_path(
                corpus_root, operation.get("payload_path"), "official payload"
            )
            if not path.is_file():
                raise FirmwareError(f"official payload is missing: {path}")
            data = path.read_bytes()
        if len(data) != size or _sha256(data) != digest:
            raise FirmwareError("official recipe payload size or hash mismatch")
        normalization = operation.get("normalization")
        if normalization is not None:
            data = restore_normalization(
                data,
                normalization,
                context=f"official recipe operation {expected_order}",
            )
            source_sha = operation.get("source_sha256")
            if _sha256(data) != source_sha:
                raise FirmwareError("official recipe restored hash mismatch")
        result.append((
            start,
            end,
            data,
            f"official:{recipe_path.name}:{expected_order}",
        ))
    return result


def apply_official_recipe(path: Path, baseline: bytes) -> bytes:
    """Apply one validated official recipe to a caller-supplied baseline."""
    result = bytearray(baseline)
    for start, end, data, owner in _official_operations(path):
        if start < 0 or end > len(result) or end <= start:
            raise FirmwareError(f"{owner} exceeds the supplied baseline")
        result[start:end] = data
    return bytes(result)


def _dump_operations(
    recipe_path: Path, roles: object,
) -> list[tuple[int, int, bytes, str]]:
    document = _load_json(recipe_path, "dump recipe")
    slices = document.get("slices")
    if not isinstance(slices, list):
        raise FirmwareError("dump recipe has no slices array")
    selected_roles: set[str] | None
    if roles is None:
        selected_roles = None
    elif (
        isinstance(roles, list)
        and roles
        and all(isinstance(item, str) for item in roles)
    ):
        selected_roles = set(roles)
    else:
        raise FirmwareError("dump recipe roles must be a non-empty string array")
    full = reconstruct_recipe(recipe_path)
    result: list[tuple[int, int, bytes, str]] = []
    seen: set[str] = set()
    for item in slices:
        role = item.get("role")
        if selected_roles is not None and role not in selected_roles:
            continue
        source_range = item.get("source_range")
        if not isinstance(source_range, dict):
            raise FirmwareError("dump recipe slice has no source range")
        start = source_range.get("from")
        end = source_range.get("to_exclusive")
        if not isinstance(start, int) or not isinstance(end, int):
            raise FirmwareError("dump recipe slice range is invalid")
        seen.add(role)
        result.append((
            start,
            end,
            full[start:end],
            f"dump:{recipe_path.name}:{role}",
        ))
    if selected_roles is not None and seen != selected_roles:
        missing = ", ".join(sorted(selected_roles - seen))
        raise FirmwareError(f"dump recipe does not contain role(s): {missing}")
    return result


def compose_manifest(path: Path) -> CompositionResult:
    manifest = _load_json(path, "composition manifest")
    if (
        manifest.get("schema") != SCHEMA
        or manifest.get("schema_version") != SCHEMA_VERSION
    ):
        raise FirmwareError(f"unsupported composition schema: {path}")
    layout_value = manifest.get("layout")
    if not isinstance(layout_value, dict) or not isinstance(
        layout_value.get("name"), str
    ):
        raise FirmwareError("composition manifest has no layout name")
    layout_file = layout_value.get("catalog_path")
    loaded = load_layout(
        layout_value["name"],
        _relative_path(path.parent, layout_file, "layout catalog")
        if layout_file is not None else None,
    )
    if (
        layout_value.get("length", loaded.layout.length) != loaded.layout.length
        or layout_value.get("base", loaded.layout.base) != loaded.layout.base
    ):
        raise FirmwareError("composition layout identity does not match catalog")
    entries = manifest.get("operations")
    if not isinstance(entries, list) or not entries:
        raise FirmwareError("composition manifest has no operations")

    planned: list[tuple[int, int, bytes, str]] = []
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict):
            raise FirmwareError(f"composition operation {index} is invalid")
        kind = entry.get("kind")
        if kind == "dump-recipe":
            recipe = _relative_path(
                path.parent, entry.get("recipe"), "dump recipe"
            )
            planned.extend(_dump_operations(recipe, entry.get("roles")))
        elif kind == "official-package":
            recipe = _relative_path(
                path.parent, entry.get("recipe"), "official package recipe"
            )
            planned.extend(_official_operations(recipe))
        elif kind == "custom":
            start, end = _validated_range(
                entry.get("range"), loaded, entry.get("role")
            )
            payload_path = _relative_path(
                path.parent, entry.get("path"), "custom payload"
            )
            if not payload_path.is_file():
                raise FirmwareError(f"custom payload is missing: {payload_path}")
            data = payload_path.read_bytes()
            digest = entry.get("sha256")
            if (
                not isinstance(digest, str)
                or len(digest) != 64
                or _sha256(data) != digest
                or len(data) != end - start
            ):
                raise FirmwareError("custom payload size or pinned hash mismatch")
            planned.append((start, end, data, f"custom:{payload_path.name}"))
        elif kind == "erased":
            start, end = _validated_range(
                entry.get("range"), loaded, entry.get("role")
            )
            planned.append((
                start, end, b"\xFF" * (end - start), f"erased:{index}"
            ))
        else:
            raise FirmwareError(
                f"composition operation {index} has unsupported kind {kind!r}"
            )

    image = bytearray(loaded.layout.length)
    coverage = bytearray(loaded.layout.length)
    owners: list[str | None] = [None] * loaded.layout.length
    overlaps: list[dict[str, Any]] = []
    for start, end, data, owner in planned:
        if (
            start < 0
            or end > loaded.layout.length
            or end <= start
            or len(data) != end - start
        ):
            raise FirmwareError(f"composition source {owner} has invalid range")
        cursor = start
        while cursor < end:
            previous = owners[cursor]
            if previous is None:
                cursor += 1
                continue
            overlap_start = cursor
            while cursor < end and owners[cursor] == previous:
                cursor += 1
            overlaps.append({
                "range": {
                    "from": overlap_start,
                    "to_exclusive": cursor,
                    "length": cursor - overlap_start,
                },
                "previous": previous,
                "replacement": owner,
            })
        image[start:end] = data
        coverage[start:end] = b"\x01" * (end - start)
        owners[start:end] = [owner] * (end - start)

    uncovered = []
    cursor = 0
    while cursor < len(coverage):
        while cursor < len(coverage) and coverage[cursor]:
            cursor += 1
        start = cursor
        while cursor < len(coverage) and not coverage[cursor]:
            cursor += 1
        if start < cursor:
            uncovered.append((start, cursor))
    if uncovered:
        rendered = ", ".join(
            f"0x{start:06X}..0x{end - 1:06X}" for start, end in uncovered
        )
        raise FirmwareError(f"composition leaves uncovered ranges: {rendered}")

    result = bytes(image)
    digest = _sha256(result)
    expected = manifest.get("output_sha256")
    if expected is not None and expected != digest:
        raise FirmwareError(
            f"composition SHA-256 {digest} does not match {expected}"
        )
    return CompositionResult(result, digest, tuple(overlaps), len(planned))


def command_compose(args: argparse.Namespace) -> None:
    result = compose_manifest(args.manifest)
    _atomic_write(args.output, result.image, args.force)
    print(
        f"written: {args.output} ({len(result.image)} bytes, "
        f"sha256={result.sha256}, operations={result.operations}, "
        f"overlaps={len(result.overlaps)})"
    )
