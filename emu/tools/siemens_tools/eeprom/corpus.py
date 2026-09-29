from __future__ import annotations

import hashlib
import struct
from pathlib import Path
from typing import Any, Iterable

from .storage import Block, load_eeprom_source


CORPUS_SCHEMA = "siemens-tools/eeprom-corpus"
CORPUS_SCHEMA_VERSION = 1
SOURCE_SUFFIXES = {".bin", ".map"}


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _inside_nested_split_output(root: Path, path: Path) -> bool:
    relative = path.relative_to(root)
    return any(part.endswith(".split") for part in relative.parts[:-1])


def discover_sources(roots: Iterable[Path]) -> list[Path]:
    """Discover supported EEPROM source candidates once, in stable path order."""
    found: dict[Path, Path] = {}
    roots = list(roots)
    if not roots:
        raise ValueError("at least one corpus input path is required")
    for root in roots:
        if not root.exists():
            raise ValueError(f"corpus input does not exist: {root}")
        if root.is_file():
            candidates = [root] if root.suffix.lower() in SOURCE_SUFFIXES else []
        elif root.is_dir():
            candidates = (
                path for path in root.rglob("*")
                if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES
                and not _inside_nested_split_output(root, path)
            )
        else:
            candidates = []
        for path in candidates:
            found.setdefault(path.resolve(), path)
    if not found:
        raise ValueError("corpus inputs contain no .bin or .map candidates")
    return sorted(found.values(), key=lambda path: path.as_posix())


def _block_json(block: Block) -> dict[str, Any]:
    return {
        "id": block.block_id,
        "id_hex": f"0x{block.block_id:04X}",
        "length": block.length,
        "payload_sha256": _sha256(block.payload),
        "raw_hex": block.payload.hex(),
        "memory_class": block.memory_class,
        "version": block.version,
        "marker": f"0x{block.marker:04X}" if block.marker is not None else None,
        "linear": f"0x{block.linear:06X}" if block.linear is not None else None,
        "file_offset": (
            f"0x{block.file_off:06X}" if block.file_off is not None else None
        ),
        "directory_offset": (
            f"0x{block.dir_off:06X}" if block.dir_off is not None else None
        ),
        "name": block.name or None,
    }


def _profile_sha256(block_ids: list[int], inventory: dict[int, Block]) -> str:
    digest = hashlib.sha256(b"siemens-eeprom-profile-v1\0")
    for block_id in block_ids:
        digest.update(struct.pack(">I", block_id))
        block = inventory.get(block_id)
        if block is None:
            digest.update(b"\0")
        else:
            digest.update(b"\1")
            digest.update(struct.pack(">I", block.length))
            digest.update(block.payload)
    return digest.hexdigest()


def _invariant_spans(payloads: list[bytes]) -> tuple[list[dict[str, Any]], list[int]]:
    max_length = max((len(payload) for payload in payloads), default=0)
    invariant: list[bool] = []
    for offset in range(max_length):
        values = {
            payload[offset] if offset < len(payload) else None
            for payload in payloads
        }
        invariant.append(len(values) == 1 and None not in values)

    spans: list[dict[str, Any]] = []
    offset = 0
    while offset < max_length:
        if not invariant[offset]:
            offset += 1
            continue
        end = offset + 1
        while end < max_length and invariant[end]:
            end += 1
        spans.append({
            "start": offset,
            "end": end,
            "hex": payloads[0][offset:end].hex(),
        })
        offset = end
    varying = [offset for offset, fixed in enumerate(invariant) if not fixed]
    return spans, varying


def _word_views(payload: bytes) -> dict[str, list[int]]:
    if len(payload) % 2:
        return {}
    count = len(payload) // 2
    return {
        "u16_le": list(struct.unpack(f"<{count}H", payload)),
        "i16_le": list(struct.unpack(f"<{count}h", payload)),
    }


def analyze_corpus(roots: Iterable[Path], block_ids: Iterable[int]) -> dict[str, Any]:
    """Parse a source corpus and cluster selected EEPROM block payloads."""
    roots = list(roots)
    selected = list(dict.fromkeys(block_ids))
    if not selected:
        raise ValueError("at least one --block is required")
    if any(block_id < 0 or block_id > 0xFFFF for block_id in selected):
        raise ValueError("EEPROM block ids must be in range 0..65535")

    candidates = discover_sources(roots)
    source_rows: list[dict[str, Any]] = []
    parsed: list[tuple[dict[str, Any], dict[int, Block]]] = []
    for path in candidates:
        row: dict[str, Any] = {"path": path.as_posix()}
        try:
            raw = path.read_bytes()
            row["size"] = len(raw)
            row["sha256"] = _sha256(raw)
            kind, inventory = load_eeprom_source(path)
        except (OSError, UnicodeError, ValueError) as exc:
            row.update({"status": "error", "error": str(exc)})
            source_rows.append(row)
            continue

        blocks = {
            str(block_id): _block_json(inventory[block_id])
            for block_id in selected if block_id in inventory
        }
        row.update({
            "status": "parsed",
            "kind": kind,
            "inventory_count": len(inventory),
            "selected_count": len(blocks),
            "complete": len(blocks) == len(selected),
            "blocks": blocks,
        })
        row["profile_sha256"] = (
            _profile_sha256(selected, inventory) if blocks else None
        )
        source_rows.append(row)
        parsed.append((row, inventory))

    block_rows: list[dict[str, Any]] = []
    for block_id in selected:
        variant_groups: dict[str, dict[str, Any]] = {}
        missing: list[str] = []
        for source, inventory in parsed:
            block = inventory.get(block_id)
            if block is None:
                missing.append(source["path"])
                continue
            payload_sha = _sha256(block.payload)
            group = variant_groups.setdefault(payload_sha, {
                "payload_sha256": payload_sha,
                "length": block.length,
                "raw_hex": block.payload.hex(),
                **_word_views(block.payload),
                "occurrences": [],
            })
            group["occurrences"].append({
                "path": source["path"],
                "source_sha256": source["sha256"],
                "kind": source["kind"],
                "memory_class": block.memory_class,
                "version": block.version,
            })

        variants = sorted(
            variant_groups.values(), key=lambda variant: variant["payload_sha256"]
        )
        for variant in variants:
            variant["occurrences"].sort(key=lambda item: item["path"])
            variant["source_occurrences"] = len(variant["occurrences"])
            variant["unique_source_files"] = len({
                item["source_sha256"] for item in variant["occurrences"]
            })
            variant["memory_classes"] = sorted({
                item["memory_class"] for item in variant["occurrences"]
                if item["memory_class"] is not None
            })
            variant["versions"] = sorted({
                item["version"] for item in variant["occurrences"]
                if item["version"] is not None
            })

        payloads = [bytes.fromhex(variant["raw_hex"]) for variant in variants]
        invariant_spans, varying_offsets = _invariant_spans(payloads)
        lengths = sorted({len(payload) for payload in payloads})
        block_rows.append({
            "id": block_id,
            "id_hex": f"0x{block_id:04X}",
            "source_occurrences": sum(
                variant["source_occurrences"] for variant in variants
            ),
            "unique_payloads": len(variants),
            "lengths": lengths,
            "missing_sources": sorted(missing),
            "consensus": {
                "common_length": lengths[0] if len(lengths) == 1 else None,
                "invariant_spans": invariant_spans,
                "varying_offsets": varying_offsets,
            },
            "variants": variants,
        })

    profile_groups: dict[str, dict[str, Any]] = {}
    for source, inventory in parsed:
        if not source["blocks"]:
            continue
        profile_sha = source["profile_sha256"]
        profile = profile_groups.setdefault(profile_sha, {
            "profile_sha256": profile_sha,
            "complete": source["complete"],
            "blocks": {
                str(block_id): (
                    _sha256(inventory[block_id].payload)
                    if block_id in inventory else None
                )
                for block_id in selected
            },
            "sources": [],
        })
        profile["sources"].append(source["path"])
    profiles = sorted(
        profile_groups.values(), key=lambda profile: profile["profile_sha256"]
    )
    for profile in profiles:
        profile["sources"].sort()
        profile["source_occurrences"] = len(profile["sources"])

    parsed_count = sum(row["status"] == "parsed" for row in source_rows)
    target_bearing = sum(
        row["status"] == "parsed" and row["selected_count"] > 0
        for row in source_rows
    )
    complete_occurrences = sum(
        row["status"] == "parsed" and row["complete"] for row in source_rows
    )
    return {
        "schema": CORPUS_SCHEMA,
        "schema_version": CORPUS_SCHEMA_VERSION,
        "roots": [root.as_posix() for root in roots],
        "requested_blocks": selected,
        "summary": {
            "candidates": len(source_rows),
            "parsed": parsed_count,
            "parse_failed": len(source_rows) - parsed_count,
            "target_bearing": target_bearing,
            "complete_occurrences": complete_occurrences,
            "unique_source_files": len({
                row["sha256"] for row in source_rows if "sha256" in row
            }),
            "profiles": len(profiles),
            "complete_profiles": sum(profile["complete"] for profile in profiles),
        },
        "sources": source_rows,
        "blocks": block_rows,
        "profiles": profiles,
    }


def render_corpus(report: dict[str, Any]) -> str:
    """Render a compact deterministic human-readable corpus summary."""
    summary = report["summary"]
    lines = [
        "EEPROM corpus",
        "  roots: " + ", ".join(report["roots"]),
        "  blocks: " + ", ".join(str(value) for value in report["requested_blocks"]),
        (
            f"  sources: {summary['candidates']} candidates, {summary['parsed']} parsed, "
            f"{summary['parse_failed']} failed, {summary['target_bearing']} target-bearing"
        ),
        (
            f"  profiles: {summary['profiles']} total, "
            f"{summary['complete_profiles']} complete"
        ),
        "",
        "blocks:",
        f"{'id':>6} {'occ':>5} {'variants':>8} {'lengths':>12} {'varying':>8}",
    ]
    for block in report["blocks"]:
        lengths = ",".join(str(value) for value in block["lengths"]) or "-"
        lines.append(
            f"{block['id']:6d} {block['source_occurrences']:5d} "
            f"{block['unique_payloads']:8d} {lengths:>12} "
            f"{len(block['consensus']['varying_offsets']):8d}"
        )

    lines.extend(["", "profiles:"])
    for profile in report["profiles"]:
        state = "complete" if profile["complete"] else "partial"
        lines.append(
            f"  {profile['profile_sha256'][:12]}  {state:<8} "
            f"{profile['source_occurrences']:3d} source(s)"
        )

    failures = [
        source for source in report["sources"] if source["status"] == "error"
    ]
    if failures:
        lines.extend(["", "parse failures:"])
        for source in failures:
            lines.append(f"  {source['path']}: {source['error']}")
    return "\n".join(lines) + "\n"
