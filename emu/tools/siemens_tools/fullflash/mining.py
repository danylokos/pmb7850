from __future__ import annotations

import hashlib
import itertools
import re
import struct
from pathlib import Path
from typing import Any, Iterable

from .. import eeprom
from ..firmware.inspection import detect_content_type
from ..firmware.xbi import FirmwareError, FlashLayout
from .bcore_normalization import (
    BCORE_ERASED_VALUE,
    BCORE_FIELD_OFFSET,
    BCORE_FIELD_SIZE,
)
from ..layout.catalog import load_layout_catalog
from .ranges import _true_ranges, source_layout_intersection
from .statistics import PAGE_SIZE, inspect_statistics


SCHEMA = "siemens-fullflash-info"
SCHEMA_VERSION = 3
_METADATA_FIXED = bytes.fromhex("FF 0A 50 14 14 01 00 D5 21")
_BCORE_HEADER_PREFIX = bytes.fromhex("00 01 4C 53 01 00 00 01")
_BYTE_RUNS = {
    0x00: re.compile(b"\x00+"),
    0xFF: re.compile(b"\xFF+"),
}
_FLASH_ENGINES = {
    (0x0020, 0x0017): ("ST", "m58lw064d", "exact"),
    (0x0089, 0x0016): ("Intel", "m58lw064d", "compatible"),
    (0x002C, 0x0016): ("Micron", "m58lw064d", "compatible"),
    (0x0089, 0x0017): ("Intel", "m58lw064d", "compatible"),
    (0x002C, 0x0017): ("Micron", "m58lw064d", "compatible"),
    (0x0001, 0x220C): ("AMD", "am29lv640mh", "exact"),
    (0x0001, 0x2212): ("AMD", "am29lv128mh", "exact"),
    (0x0089, 0x8854): ("Intel", "w30-64mbit-top", "exact"),
    (0x0020, 0x8810): ("ST", "w30-64mbit-top", "compatible"),
    (0x0089, 0x8856): ("Intel", "w30-128mbit-top", "exact"),
}


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _bcd_byte(value: int) -> int | None:
    high, low = value >> 4, value & 0x0F
    return high * 10 + low if high <= 9 and low <= 9 else None


def _hex_range(start: int, end: int) -> dict[str, Any]:
    return {
        "from": f"0x{start:X}",
        "to_exclusive": f"0x{end:X}",
        "length": end - start,
    }


def _int_range(start: int, end: int) -> dict[str, int]:
    return {"from": start, "to": end}


def _field(data: bytes, offset: int, size: int, pattern: str) -> str | None:
    raw = data[offset:offset + size]
    if len(raw) != size or b"\0" not in raw:
        return None
    value, padding = raw.split(b"\0", 1)
    if not value or any(padding):
        return None
    try:
        text = value.decode("ascii")
    except UnicodeDecodeError:
        return None
    return text if re.fullmatch(pattern, text) else None


def _finding(kind: str, family: str, data: bytes, start: int, end: int,
             evidence: list[str], metadata: dict[str, Any] | None = None,
             placements: list[dict[str, Any]] | None = None) -> dict[str, Any]:
    result: dict[str, Any] = {
        "kind": kind,
        "family": family,
        "file_range": _hex_range(start, end),
        "sha256": _sha256(data[start:end]),
        "evidence": evidence,
        "_placements": placements or [],
    }
    if metadata is not None:
        result["metadata"] = metadata
    return result


def _flash_metadata(data: bytes, view_base: int) -> dict[str, Any]:
    tuple_offset = view_base + 0x7FE26
    software_offset = view_base + 0x32C
    result: dict[str, Any] = {
        "metadata_view_file_offset": f"0x{view_base:X}",
        "bcore_software_version": None,
        "bcore_software_version_raw": None,
        "flash": None,
    }
    if 0 <= software_offset < len(data):
        raw = data[software_offset]
        result["bcore_software_version"] = _bcd_byte(raw)
        result["bcore_software_version_raw"] = f"0x{raw:02X}"
    if 0 <= tuple_offset <= len(data) - 4:
        manufacturer, device = struct.unpack_from("<HH", data, tuple_offset)
        vendor, engine, classification = _FLASH_ENGINES.get(
            (manufacturer, device),
            ({
                0x0001: "AMD", 0x0020: "ST", 0x002C: "Micron",
                0x0089: "Intel",
            }.get(manufacturer, "unknown"), None, "unsupported"),
        )
        result["flash"] = {
            "vendor": vendor,
            "manufacturer_id": f"0x{manufacturer:04X}",
            "device_id": f"0x{device:04X}",
            "raw": data[tuple_offset:tuple_offset + 4].hex(" "),
            "engine": engine,
            "classification": classification,
            "emulator_approximation": classification == "compatible",
        }
    return result


def scan_firmware_metadata(data: bytes) -> list[dict[str, Any]]:
    findings: list[dict[str, Any]] = []
    cursor = 0
    while True:
        fixed_at = data.find(_METADATA_FIXED, cursor)
        if fixed_at < 0:
            break
        offset = fixed_at - 1
        cursor = fixed_at + 1
        if offset < 0 or offset + 0x40 > len(data):
            continue
        header = data[offset:offset + 16]
        software = _bcd_byte(header[0])
        langpack = _field(data, offset + 0x10, 16, r"(?i:lg)[0-9]+")
        model = _field(data, offset + 0x20, 16, r"[A-Z][A-Z0-9]{1,14}")
        if (software is None or header[1:10] != _METADATA_FIXED
                or header[12:16] != b"\xFF" * 4
                or langpack is None or model is None
                or data[offset + 0x30:offset + 0x38] != b"SIEMENS\0"):
            continue
        view_base = offset - 0x7FF50
        metadata = {
            "model": model,
            "software_version": software,
            "software_version_raw": f"0x{header[0]:02X}",
            "langpack": langpack,
            "file_offset": f"0x{offset:X}",
            **_flash_metadata(data, view_base),
        }
        findings.append(_finding(
            "firmware-metadata", "firmware_metadata", data, offset,
            offset + 0x40,
            [
                "BCD software byte and fixed firmware header",
                "NUL-padded language/model fields",
                "exact SIEMENS vendor field",
            ],
            metadata,
            [{"model": model, "source_anchor": view_base,
              "region": "BCORE", "relative": 0, "model_bearing": True}],
        ))
    return findings


def _optional_ascii(data: bytes, offset: int, size: int,
                    pattern: bytes) -> str | None:
    raw = data[offset:offset + size]
    match = re.match(pattern, raw)
    return match.group(0).decode("ascii") if match else None


def _bcore_span(model: str, available: int,
                layouts: Iterable[FlashLayout]) -> int:
    lengths = {
        region.length
        for layout in layouts if _layout_matches_model(layout, model)
        for region in layout.regions if region.name == "BCORE"
    }
    if len(lengths) == 1:
        return min(available, next(iter(lengths)))
    return min(available, 0x10000)


def scan_bcore(data: bytes, layouts: Iterable[FlashLayout]) -> list[dict[str, Any]]:
    findings: list[dict[str, Any]] = []
    cursor = 0
    while True:
        marker = data.find(b"SIEMENS\0", cursor)
        if marker < 0:
            break
        cursor = marker + 1
        base = marker - 0x31C
        if base < 0 or base + 0x340 > len(data):
            continue
        header = data[base + 0x300:base + 0x30C]
        model = _field(data, base + 0x30C, 16, r"[A-Z][A-Z0-9]{1,14}")
        software_raw = data[base + 0x32C]
        software = _bcd_byte(software_raw)
        if (header[:8] != _BCORE_HEADER_PREFIX or header[10:12] != b"\x80\0"
                or model is None or software is None
                or (len(model) > 2 and len(set(model)) == 1)):
            continue
        span = _bcore_span(model, len(data) - base, layouts)
        codename = _optional_ascii(
            data, base + 0x104, 16, rb"[A-Za-z][A-Za-z0-9_-]{1,15}"
        )
        build_stamp = _optional_ascii(data, base + 0x114, 16, rb"[0-9]{12}")
        timestamp_match = re.search(
            rb"[0-3][0-9]\.[01][0-9]\.[0-9]{2}[0-2][0-9]:[0-5][0-9]:[0-5][0-9]",
            data[base:base + span],
        )
        timestamp = (
            timestamp_match.group(0).decode("ascii")
            if timestamp_match is not None else None
        )
        payload = data[base:base + span]
        normalized = bytearray(payload)
        if len(normalized) >= BCORE_FIELD_OFFSET + BCORE_FIELD_SIZE:
            normalized[
                BCORE_FIELD_OFFSET:BCORE_FIELD_OFFSET + BCORE_FIELD_SIZE
            ] = BCORE_ERASED_VALUE
        metadata = {
            "model": model,
            "software_version": software,
            "software_version_raw": f"0x{software_raw:02X}",
            "header_file_offset": f"0x{base + 0x300:X}",
            "structural_header_raw": header.hex(" "),
            "device_codename": codename,
            "build_stamp": build_stamp,
            "embedded_timestamp": timestamp,
            "fingerprints": {
                "raw_sha256": _sha256(payload),
                "field_330_normalized_sha256": _sha256(normalized),
                "field_330_raw": (
                    payload[0x330:0x340].hex()
                    if len(payload) >= 0x340 else None
                ),
            },
        }
        findings.append(_finding(
            "bcore", "bcore", data, base, base + span,
            [
                "fixed LS structural header at +0x300",
                "validated NUL-padded model at +0x30C",
                "exact SIEMENS marker at +0x31C",
                "valid BCD software byte at +0x32C",
            ],
            metadata,
            [{"model": model, "source_anchor": base, "region": "BCORE",
              "relative": 0, "model_bearing": True}],
        ))
    return findings


def _eeprom_geometry(data: bytes, magic: int) -> tuple[int, int] | None:
    for first, second, size in (
        (0x20000, 0x40000, 0x60000),
        (0x10000, 0x20000, 0x30000),
        (0x10000, 0x12000, 0x20000),
    ):
        if (data[magic + first:magic + first + 6] == b"EEFULL"
                and data[magic + second:magic + second + 6] == b"EEFULL"):
            return first, size
    return None


def _decode_imei(inventory: dict[int, eeprom.Block],
                 block_id: int) -> dict[str, Any]:
    if block_id not in inventory:
        return {"present": False, "imei": None, "error": "block absent"}
    try:
        decoded = eeprom.extract_block(inventory, block_id)
        imei = decoded.get("imei")
        if not imei:
            return {"present": True, "imei": None, "error": "empty IMEI"}
        return {"present": True, "imei": imei, "error": None}
    except (KeyError, ValueError, struct.error) as exc:
        return {"present": True, "imei": None, "error": str(exc)}


def scan_eeprom(data: bytes, source_phases: set[int]) -> list[dict[str, Any]]:
    findings: list[dict[str, Any]] = []
    seen: set[int] = set()
    cursor = 0
    while True:
        magic = data.find(b"EELITE", cursor)
        if magic < 0:
            break
        cursor = magic + 1
        if magic < 2 or data[magic - 2:magic] != b"\xFE\xFE":
            continue
        geometry = _eeprom_geometry(data, magic)
        if geometry is None:
            continue
        bank_stride, region_size = geometry
        bases = [
            magic - offset for offset in (0x12, 0x82)
            if (magic >= offset
                and (not source_phases
                     or (magic - offset) % 0x10000 in source_phases))
        ]
        candidates: list[tuple[int, dict[str, Any]]] = []
        for base in bases:
            if base in seen:
                continue
            try:
                located = eeprom.find_eeprom_region(
                    data, base=base, size=region_size
                )
                records = eeprom.parse_directory_records(data, located)
                inventory = eeprom.load_dump_blocks(data, located)
            except (ValueError, struct.error):
                continue
            if not records or not inventory:
                continue
            seen.add(base)
            active_records = sum(record.active for record in records)
            history_records = len(records) - active_records
            imei_76 = _decode_imei(inventory, 76)
            imei_5009 = _decode_imei(inventory, 5009)
            decoded_values = [
                value["imei"] for value in (imei_76, imei_5009)
                if value["imei"] is not None
            ]
            bank_headers = []
            bank_cursor = base
            region_end = min(len(data), base + region_size)
            while bank_cursor < region_end:
                for header_offset in (0x10, 0x80):
                    name_at = bank_cursor + header_offset + 2
                    name = data[name_at:name_at + 6]
                    if name in (b"EELITE", b"EEFULL"):
                        bank_headers.append({
                            "name": name.decode("ascii"),
                            "file_offset": (
                                f"0x{bank_cursor + header_offset:X}"
                            ),
                        })
                        break
                bank_cursor += bank_stride
            metadata = {
                "status": "parsed",
                "region_file_base": f"0x{base:X}",
                "region_linear_base": f"0x{located.region_linear_base:X}",
                "region_size": region_size,
                "bank_stride": bank_stride,
                "banks": bank_headers,
                "directory_record_count": len(records),
                "active_record_count": active_records,
                "history_record_count": history_records,
                "selected_block_count": len(inventory),
                "record_count": len(records),
                "blocks": [
                    eeprom.serialize_block(inventory[block_id])
                    for block_id in sorted(inventory)
                ],
                "imei": {
                    "76": imei_76,
                    "5009": imei_5009,
                    "consistent": (
                        decoded_values[0] == decoded_values[1]
                        if len(decoded_values) == 2 else None
                    ),
                    "errors": [
                        f"{block_id}: {value['error']}"
                        for block_id, value in (
                            (76, imei_76), (5009, imei_5009)
                        )
                        if value["error"] is not None
                    ],
                },
                "imeis": {
                    str(block_id): value["imei"]
                    for block_id, value in (
                        (76, imei_76), (5009, imei_5009)
                    )
                    if value["imei"] is not None
                },
            }
            end = min(len(data), base + region_size)
            candidate = _finding(
                "eeprom", "eeprom", data, base, end,
                [
                    "paired EELITE/EEFULL bank headers",
                    f"{len(records)} valid directory records",
                    f"{len(inventory)} selected block IDs",
                ],
                metadata,
                [{"source_anchor": base, "region": "EEPROM", "relative": 0,
                  "model_bearing": False}],
            )
            candidates.append((len(decoded_values), candidate))
        if candidates:
            best_imei_count = max(score for score, _finding in candidates)
            best = [
                finding for score, finding in candidates
                if score == best_imei_count
            ]
            findings.extend(
                best if best_imei_count and len(best) == 1
                else [finding for _score, finding in candidates]
            )
    return findings


def _scan_pattern(data: bytes, pattern: bytes) -> Iterable[int]:
    cursor = 0
    while True:
        offset = data.find(pattern, cursor)
        if offset < 0:
            return
        yield offset
        cursor = offset + 1


def scan_region_markers(data: bytes) -> list[dict[str, Any]]:
    findings: list[dict[str, Any]] = []
    for offset in _scan_pattern(data, b"T9"):
        sample = data[offset:offset + 0x40]
        if (len(sample) < 0x20 or sample[2:8] in (b"\0" * 6, b"\xFF" * 6)
                or sample[10:14] != b"S9\x01\x01"
                or sample.count(0) < 8):
            continue
        findings.append(_finding(
            "region-marker", "t9", data, offset, offset + 2,
            ["T9 header plus non-erased structured header bytes"],
            {"region": "T9", "marker": "54 39"},
            [{"source_anchor": offset, "region": "T9", "relative": 0,
              "model_bearing": False}],
        ))
    for offset in _scan_pattern(data, b"\xBB\xBB"):
        sample = data[offset:offset + 0x40]
        if not re.search(rb"(?i:lg)[0-9]+\0", sample):
            continue
        findings.append(_finding(
            "region-marker", "langpack", data, offset, offset + 2,
            ["BB BB header and nearby NUL-terminated language tag"],
            {"region": "LangPack", "marker": "bb bb"},
            [{"source_anchor": offset, "region": "LangPack", "relative": 0,
              "model_bearing": False}],
        ))
    for family, region_name, pattern in (
        ("ffs", "FFS", b"\xFE\xFEFFS"),
        ("ee_fs", "EE_FS", b"\xFE\xFEEE_FS"),
    ):
        for offset in _scan_pattern(data, pattern):
            placements = [
                {"source_anchor": offset - delta, "region": region_name,
                 "relative": 0, "model_bearing": False}
                for delta in (0x10, 0x80) if offset >= delta
            ]
            findings.append(_finding(
                "region-marker", family, data, offset,
                offset + len(pattern),
                [f"exact {region_name} bank marker"],
                {"region": region_name, "marker": pattern.hex(" ")},
                placements,
            ))
    return findings


def scan_reset_candidates(data: bytes,
                          source_phases: set[int]) -> list[dict[str, Any]]:
    findings: list[dict[str, Any]] = []
    for phase in sorted(source_phases):
        for offset in range(phase, len(data) - 3, 0x10000):
            if data[offset] != 0xFA:
                continue
            target = data[offset + 1] << 16 | int.from_bytes(
                data[offset + 2:offset + 4], "little"
            )
            findings.append(_finding(
                "reset-jmps", "reset", data, offset, offset + 4,
                ["C166 JMPS opcode at a content-derived 64 KiB boundary"],
                {
                    "target": f"0x{target:06X}",
                    "bytes": data[offset:offset + 4].hex(" "),
                },
                [{"source_anchor": offset, "region": "__RESET__",
                  "relative": 0, "model_bearing": False}],
            ))
    return findings


def _source_phases(findings: Iterable[dict[str, Any]]) -> set[int]:
    return {
        placement["source_anchor"] % 0x10000
        for finding in findings
        if finding["family"] in {
            "firmware_metadata", "bcore", "t9", "langpack"
        }
        for placement in finding["_placements"]
    }


def scan_findings(data: bytes, layouts: Iterable[FlashLayout]) -> list[dict[str, Any]]:
    findings = [
        *scan_firmware_metadata(data),
        *scan_bcore(data, layouts),
        *scan_region_markers(data),
    ]
    source_phases = _source_phases(findings)
    findings.extend(scan_eeprom(data, source_phases))
    findings.extend(scan_reset_candidates(data, source_phases or {0}))
    return sorted(findings, key=lambda finding: (
        int(finding["file_range"]["from"], 16), finding["kind"]
    ))


def _layout_matches_model(layout: FlashLayout, model: str) -> bool:
    folded = model.casefold()
    return any(token.casefold() == folded for token in layout.name.split("/"))


def _regions(layout: FlashLayout, requested: str) -> list[Any]:
    if requested == "__RESET__":
        return [] if layout.reset_offset is None else [
            type("_Reset", (), {"name": "reset", "offset": layout.reset_offset})()
        ]
    if requested == "FFS":
        return [
            region for region in layout.regions
            if region.name == "FFS(A)" or region.name.startswith("FFS_")
        ]
    return [region for region in layout.regions if region.name == requested]


def _candidate_groups(findings: list[dict[str, Any]],
                      layouts: Iterable[FlashLayout]) -> list[dict[str, Any]]:
    groups: dict[tuple[str, int], dict[str, Any]] = {}
    for finding_index, finding in enumerate(findings):
        for hint in finding["_placements"]:
            model = hint.get("model")
            for layout in layouts:
                if model is not None and not _layout_matches_model(layout, model):
                    continue
                for region in _regions(layout, hint["region"]):
                    shift = region.offset - hint["source_anchor"]
                    key = (layout.name, shift)
                    group = groups.setdefault(key, {
                        "layout": layout.name,
                        "shift": shift,
                        "anchors": [],
                        "families": set(),
                        "model_bearing": False,
                        "strength": 0,
                    })
                    anchor = {
                        "finding": finding_index,
                        "family": finding["family"],
                        "kind": finding["kind"],
                        "file_offset": finding["file_range"]["from"],
                        "declared_region": region.name,
                    }
                    if anchor not in group["anchors"]:
                        group["anchors"].append(anchor)
                    group["families"].add(finding["family"])
                    group["model_bearing"] |= bool(hint["model_bearing"])
                    group["strength"] = max(
                        group["strength"],
                        {
                            "firmware_metadata": 3,
                            "bcore": 3,
                            "eeprom": 3,
                            "reset": 2,
                        }.get(finding["family"], 1),
                    )
    result = []
    for group in groups.values():
        group["families"] = sorted(group["families"])
        group["independent_family_count"] = len(group["families"])
        result.append(group)
    return sorted(result, key=lambda item: (
        item["layout"], item["shift"], item["families"]
    ))


def _select_mapping(candidates: list[dict[str, Any]], *,
                    explicit_layout: bool,
                    flash_address: int | None,
                    file_offset: int,
                    layouts: dict[str, FlashLayout]) -> tuple[
                        dict[str, Any] | None, str, list[str]]:
    warnings: list[str] = []
    model_candidates = [
        candidate for candidate in candidates if candidate["model_bearing"]
    ]
    if model_candidates:
        highest_identity = max(
            len({"bcore", "firmware_metadata"} & set(candidate["families"]))
            for candidate in model_candidates
        )
        eligible = [
            candidate for candidate in model_candidates
            if len({"bcore", "firmware_metadata"} & set(candidate["families"]))
            == highest_identity
        ]
    else:
        eligible = [
            candidate for candidate in candidates
            if candidate["independent_family_count"] >= 2
        ]
    if explicit_layout and not eligible and candidates:
        highest = max(candidate["strength"] for candidate in candidates)
        eligible = [
            candidate for candidate in candidates
            if candidate["strength"] == highest
        ]
    if flash_address is not None:
        layout_names = (
            set(layouts) if explicit_layout
            else {candidate["layout"] for candidate in eligible}
        )
        if len(layout_names) != 1:
            return None, "unresolved", [
                "explicit address needs one selected or model-identified layout"
            ]
        layout_name = next(iter(layout_names))
        layout = layouts[layout_name]
        if not layout.base <= flash_address < layout.base + layout.length:
            raise FirmwareError(
                f"flash address 0x{flash_address:X} is outside "
                f"layout {layout.name}"
            )
        shift = flash_address - layout.base - file_offset
        return {
            "layout": layout_name,
            "shift": shift,
            "anchors": [],
            "families": ["explicit"],
            "independent_family_count": 1,
            "model_bearing": False,
            "selection": "explicit-address",
        }, "mapped", warnings
    unique = {(item["layout"], item["shift"]): item for item in eligible}
    if len(unique) == 1:
        selected = next(iter(unique.values()))
        selected = dict(selected)
        selected["selection"] = "anchor-consensus"
        return selected, "mapped", warnings
    if not eligible:
        warnings.append("no placement candidate met the anchor threshold")
    else:
        warnings.append(
            f"{len(unique)} placement candidates remain ambiguous"
        )
    return None, "unresolved", warnings


def _representation(data_length: int, layout: FlashLayout | None,
                    selected: dict[str, Any] | None,
                    repeated: bool = False) -> dict[str, Any]:
    if layout is None or selected is None:
        return {
            "kind": "repeated-container" if repeated else "raw-capture",
            "status": "unresolved",
        }
    chip_mapping = selected.get("chip_mapping")
    if chip_mapping is not None:
        return {
            "kind": "exact-fullflash",
            "status": "mapped",
            "source_range": _int_range(0, data_length),
            "layout_range": _int_range(0, layout.length),
            "native_range": _int_range(
                layout.base, layout.base + layout.length
            ),
            "extra_input_bytes": 0,
            "chip_order": chip_mapping["order"],
            "chip_offsets": chip_mapping["source_offsets"],
        }
    source_start, source_end, layout_start, layout_end = (
        source_layout_intersection(
            data_length, layout.length, selected["shift"]
        )
    )
    outside = source_start + (data_length - source_end)
    missing_prefix = layout_start
    missing_suffix = layout.length - layout_end
    if outside:
        kind = "embedded-view"
    elif not missing_prefix and not missing_suffix:
        kind = "exact-fullflash"
    elif missing_prefix and not missing_suffix:
        kind = "suffix-capture"
    elif not missing_prefix and missing_suffix:
        kind = "prefix-capture"
    else:
        kind = "interior-slice"
    matching_regions = [
        region for region in layout.regions
        if layout_start == region.offset and layout_end == region.end
    ]
    if not outside and len(matching_regions) == 1:
        kind = "standalone-region"
    return {
        "kind": kind,
        "status": "mapped",
        "source_range": _int_range(source_start, source_end),
        "layout_range": _int_range(layout_start, layout_end),
        "native_range": _int_range(
            layout.base + layout_start, layout.base + layout_end
        ),
        "extra_input_bytes": outside,
    }


def _materialize(data: bytes, layout: FlashLayout, shift: int,
                 chip_mapping: dict[str, Any] | None = None,
                 ) -> tuple[bytes, bytes]:
    image = bytearray(b"\xFF") * layout.length
    covered = bytearray(layout.length)
    if chip_mapping is not None:
        for segment in chip_mapping["segments"]:
            source_start = segment["source"]["from"]
            source_end = segment["source"]["to"]
            layout_start = segment["layout"]["from"]
            layout_end = segment["layout"]["to"]
            image[layout_start:layout_end] = data[source_start:source_end]
            covered[layout_start:layout_end] = b"\x01" * (
                layout_end - layout_start
            )
        return bytes(image), bytes(covered)
    source_start, source_end, layout_start, layout_end = (
        source_layout_intersection(len(data), layout.length, shift)
    )
    image[layout_start:layout_end] = data[source_start:source_end]
    covered[layout_start:layout_end] = b"\x01" * (layout_end - layout_start)
    return bytes(image), bytes(covered)


def _resolve_chip_mapping(
        data: bytes, layout: FlashLayout,
        metadata_instances: list[dict[str, Any]],
) -> dict[str, Any] | None:
    if not layout.chips:
        return None
    if len(data) != layout.length:
        return None
    reset_owner = next(chip for chip in layout.chips if chip.owns_reset)
    metadata_owner = next(chip for chip in layout.chips if chip.owns_metadata)
    candidates: list[dict[str, Any]] = []
    for source_order in itertools.permutations(layout.chips):
        source_offsets: dict[str, int] = {}
        cursor = 0
        for chip in source_order:
            source_offsets[chip.role] = cursor
            cursor += chip.length
        reset_source = (
            source_offsets[reset_owner.role]
            + layout.reset_offset - reset_owner.offset
        )
        reset = data[reset_source:reset_source + 4]
        if len(reset) != 4 or reset[0] != 0xFA:
            continue
        target = reset[1] << 16 | int.from_bytes(reset[2:4], "little")
        owner_start = layout.base + reset_owner.offset
        owner_end = layout.base + reset_owner.end
        if not owner_start <= target < owner_end:
            continue
        metadata_source = source_offsets[metadata_owner.role]
        matching_metadata = [
            item for item in metadata_instances
            if int(item["metadata_view_file_offset"], 16) == metadata_source
            and _layout_matches_model(layout, item["model"])
        ]
        if not matching_metadata:
            continue
        candidates.append({
            "order": (
                f"{source_order[0].role}-first"
                if len(source_order) == 2 else source_order[0].role
            ),
            "source_offsets": source_offsets,
            "segments": [
                {
                    "role": chip.role,
                    "source": _int_range(
                        source_offsets[chip.role],
                        source_offsets[chip.role] + chip.length,
                    ),
                    "layout": _int_range(chip.offset, chip.end),
                }
                for chip in sorted(layout.chips, key=lambda item: item.offset)
            ],
        })
    if len(candidates) != 1:
        qualifier = "missing" if not candidates else "ambiguous"
        raise FirmwareError(
            f"{qualifier} dual-chip order for layout {layout.name!r}"
        )
    return candidates[0]


def _source_anchor_layout_offset(
        source_offset: int, shift: int,
        chip_mapping: dict[str, Any] | None,
) -> int | None:
    if chip_mapping is None:
        return source_offset + shift
    for segment in chip_mapping["segments"]:
        source = segment["source"]
        if source["from"] <= source_offset < source["to"]:
            return (
                segment["layout"]["from"]
                + source_offset - source["from"]
            )
    return None


def _longest_run(data: bytes, value: int) -> int:
    return max(
        (match.end() - match.start()
         for match in _BYTE_RUNS[value].finditer(data)),
        default=0,
    )


def _region_rows(
        image: bytes, covered: bytes, layout: FlashLayout,
        findings: list[dict[str, Any]], shift: int,
        chip_mapping: dict[str, Any] | None = None,
) -> list[dict[str, Any]]:
    rows = []
    for region in layout.regions:
        mask = covered[region.offset:region.end]
        count = sum(mask)
        state = "missing" if count == 0 else "partial"
        chunks = []
        covered_source_ranges = []
        for start, end in _true_ranges(mask):
            layout_from = region.offset + start
            layout_to = region.offset + end
            chunks.append(image[layout_from:layout_to])
            if chip_mapping is None:
                covered_source_ranges.append(_int_range(
                    layout_from - shift, layout_to - shift
                ))
            else:
                for segment in chip_mapping["segments"]:
                    segment_layout = segment["layout"]
                    overlap_start = max(layout_from, segment_layout["from"])
                    overlap_end = min(layout_to, segment_layout["to"])
                    if overlap_start >= overlap_end:
                        continue
                    source_start = (
                        segment["source"]["from"]
                        + overlap_start - segment_layout["from"]
                    )
                    covered_source_ranges.append(_int_range(
                        source_start, source_start + overlap_end - overlap_start
                    ))
        row: dict[str, Any] = {
            "name": region.name,
            "layout_range": _int_range(region.offset, region.end),
            "native_range": _int_range(
                layout.base + region.offset, layout.base + region.end
            ),
            "length": region.length,
            "covered": count,
            "coverage_percent": round(count * 100 / region.length, 2),
            "covered_source_ranges": covered_source_ranges,
            "state": state,
            "signature_status": "not-found",
        }
        expected_families = {
            "BCORE": {"bcore", "firmware_metadata"},
            "T9": {"t9"},
            "LangPack": {"langpack"},
            "EE_FS": {"ee_fs"},
            "EEPROM": {"eeprom"},
        }
        if region.name == "FFS(A)" or region.name.startswith("FFS_"):
            expected_families[region.name] = {"ffs"}
        families = expected_families.get(region.name, set())
        if any(
            finding["family"] in families
            and any(
                placement.get("region") in (region.name, "FFS")
                and _source_anchor_layout_offset(
                    placement["source_anchor"], shift, chip_mapping
                ) == region.offset
                for placement in finding["_placements"]
            )
            for finding in findings
        ):
            row["signature_status"] = "validated"
        if count:
            payload = b"".join(chunks)
            non_ff = sum(byte != 0xFF for byte in payload)
            row.update({
                "non_ff": non_ff,
                "non_ff_percent": round(non_ff * 100 / count, 2),
                "non_zero": sum(byte != 0 for byte in payload),
                "longest_ff_run": _longest_run(payload, 0xFF),
                "longest_zero_run": _longest_run(payload, 0),
            })
            if count == region.length:
                row["state"] = "programmed" if non_ff else "erased"
        rows.append(row)
    return rows


def _reset_state(image: bytes, covered: bytes,
                 layout: FlashLayout) -> dict[str, Any]:
    if layout.reset_offset is None:
        return {"status": "not-declared"}
    offset = layout.reset_offset
    result: dict[str, Any] = {
        "layout_offset": f"0x{offset:X}",
        "linear_address": f"0x{layout.base + offset:06X}",
    }
    if not all(covered[offset:offset + 4]):
        result["status"] = "not-covered"
        return result
    reset = image[offset:offset + 4]
    result["bytes"] = reset.hex(" ")
    if reset == b"\xFF" * 4:
        result["status"] = "erased"
        return result
    if reset[0] != 0xFA:
        result["status"] = "invalid-opcode"
        return result
    target = reset[1] << 16 | int.from_bytes(reset[2:4], "little")
    target_offset = target - layout.base
    result["jmps_target"] = f"0x{target:06X}"
    if target_offset < 0 or target_offset >= layout.length:
        result["status"] = "target-outside-layout"
    elif not covered[target_offset]:
        result["status"] = "target-not-covered"
    elif image[target_offset] == 0xFF:
        result["status"] = "target-erased"
    else:
        result["status"] = "valid"
    return result


def _cold_boot(reset: dict[str, Any],
               regions: list[dict[str, Any]]) -> dict[str, Any]:
    reasons = []
    if reset["status"] == "not-declared":
        return {"status": "unknown", "reasons": ["reset-offset-not-declared"],
                "risks": []}
    if reset["status"] != "valid":
        reasons.append(f"reset-{reset['status']}")
    bcore = [row for row in regions if row["name"] == "BCORE"]
    if len(bcore) != 1:
        return {"status": "unknown",
                "reasons": reasons + ["bcore-region-not-declared-once"],
                "risks": []}
    if bcore[0]["state"] != "programmed":
        reasons.append(f"bcore-{bcore[0]['state']}")
    risks = [
        {"region": row["name"], "state": row["state"]}
        for row in regions
        if row["name"] != "BCORE" and row["state"] != "programmed"
    ]
    return {
        "status": "not-feasible" if reasons else "feasible",
        "reasons": reasons,
        "risks": risks,
    }


def _public_finding(finding: dict[str, Any]) -> dict[str, Any]:
    return {key: value for key, value in finding.items()
            if not key.startswith("_")}


def mine_fullflash(data: bytes, *, layout: FlashLayout | None = None,
                   layout_file: Path | None = None,
                   flash_address: int | None = None,
                   file_offset: int = 0) -> dict[str, Any]:
    if flash_address is None and file_offset:
        raise FirmwareError("--file-offset requires --flash-address")
    if flash_address is not None:
        if not 0 <= file_offset < len(data):
            raise FirmwareError("--file-offset must identify an input byte")
        if layout is not None and not (
            layout.base <= flash_address < layout.base + layout.length
        ):
            raise FirmwareError(
                f"flash address 0x{flash_address:X} is outside "
                f"layout {layout.name}"
            )
    try:
        content_type = detect_content_type(data)
    except FirmwareError:
        content_type = "bin"
    if data.startswith(b"MRT"):
        content_type = "martech-backup"
    catalog = load_layout_catalog(layout_file)
    layouts = (layout,) if layout is not None else catalog.layouts
    findings = scan_findings(data, catalog.layouts)
    candidates = _candidate_groups(findings, layouts)
    layout_by_name = {item.name: item for item in layouts}
    selected, status, warnings = _select_mapping(
        candidates,
        explicit_layout=layout is not None,
        flash_address=flash_address,
        file_offset=file_offset,
        layouts=layout_by_name,
    )
    selected_layout = (
        layout_by_name[selected["layout"]] if selected is not None else None
    )
    bcore_findings = [
        finding["metadata"] for finding in findings
        if finding["kind"] == "bcore"
    ]
    eeprom_findings = [
        finding["metadata"] for finding in findings
        if finding["kind"] == "eeprom"
    ]
    selected_bcore = (
        bcore_findings[0] if len(bcore_findings) == 1 else None
    )
    selected_eeprom = (
        eeprom_findings[0] if len(eeprom_findings) == 1 else None
    )
    result: dict[str, Any] = {
        "schema": SCHEMA,
        "schema_version": SCHEMA_VERSION,
        "input": {
            "size": len(data),
            "sha256": _sha256(data),
            "content_type": content_type,
        },
        "size": len(data),
        "sha256": _sha256(data),
        "content_type": content_type,
        "representation": _representation(
            len(data), selected_layout, selected,
            repeated=(
                len(data) > 0 and len(data) % 2 == 0
                and data[:len(data) // 2] == data[len(data) // 2:]
            ),
        ),
        "findings": [_public_finding(finding) for finding in findings],
        "layout_resolution": {
            "status": status,
            "candidates": candidates,
            "selected": selected,
        },
        "layout_status": status,
        "warnings": warnings,
        "metadata_instances": [
            finding["metadata"] for finding in findings
            if finding["kind"] == "firmware-metadata"
        ],
        "bcore": {
            "findings": bcore_findings,
            "selected": selected_bcore,
        },
        "eeprom": ({
            **selected_eeprom,
            "findings": eeprom_findings,
            "selected": selected_eeprom,
        } if selected_eeprom is not None else {
            "status": "ambiguous" if eeprom_findings else "not-found",
            "findings": eeprom_findings,
            "selected": None,
        }),
        "statistics": {"status": "layout-unresolved"},
        "reset": {"status": "layout-unresolved"},
        "reset_vector": {"status": "layout-unresolved"},
    }
    if layout is not None:
        result["layout"] = {
            "name": layout.name,
            "base": f"0x{layout.base:X}",
            "length": layout.length,
        }
    result["metadata"] = (
        result["metadata_instances"][0]
        if len(result["metadata_instances"]) == 1 else None
    )
    if selected_layout is None or selected is None or content_type != "bin":
        result["cold_boot"] = {
            "status": "unknown",
            "reasons": [
                "input-is-not-raw-flash" if content_type != "bin"
                else "layout-alignment-unresolved"
            ],
            "risks": [],
        }
        return result
    chip_mapping = _resolve_chip_mapping(
        data, selected_layout, result["metadata_instances"]
    )
    if chip_mapping is not None:
        selected["chip_mapping"] = chip_mapping
        result["representation"] = _representation(
            len(data), selected_layout, selected
        )
    result["layout"] = {
        "name": selected_layout.name,
        "base": f"0x{selected_layout.base:X}",
        "length": selected_layout.length,
    }
    image, covered = _materialize(
        data, selected_layout, selected["shift"], chip_mapping
    )
    covered_ranges = _true_ranges(covered)
    missing_ranges = _true_ranges(bytes(not value for value in covered))
    result["coverage"] = {
        "covered_ranges": [_hex_range(start, end)
                           for start, end in covered_ranges],
        "missing_ranges": [_hex_range(start, end)
                           for start, end in missing_ranges],
        "covered_bytes": sum(covered),
        "missing_bytes": selected_layout.length - sum(covered),
    }
    result["covered_ranges"] = result["coverage"]["covered_ranges"]
    result["missing_ranges"] = result["coverage"]["missing_ranges"]
    result["regions"] = _region_rows(
        image, covered, selected_layout, findings, selected["shift"],
        chip_mapping,
    )
    statistic_offset = selected_layout.statistic_offset
    if statistic_offset is None:
        result["statistics"] = {"status": "unsupported-layout"}
    elif not all(covered[statistic_offset:statistic_offset + PAGE_SIZE]):
        result["statistics"] = {
            "status": "incomplete",
            "layout_offset": statistic_offset,
            "native_address": selected_layout.base + statistic_offset,
        }
    else:
        result["statistics"] = {
            **inspect_statistics(
                image[statistic_offset:statistic_offset + PAGE_SIZE]
            ),
            "layout_offset": statistic_offset,
            "native_address": selected_layout.base + statistic_offset,
        }
    result["reset"] = _reset_state(image, covered, selected_layout)
    result["reset_vector"] = result["reset"]
    result["cold_boot"] = _cold_boot(result["reset"], result["regions"])
    mapped_bcores = []
    mapped_eeproms = []
    for finding in findings:
        for placement in finding["_placements"]:
            declared_offsets = {
                region.offset for region in _regions(
                    selected_layout, placement["region"]
                )
            }
            anchor = _source_anchor_layout_offset(
                placement["source_anchor"], selected["shift"], chip_mapping
            )
            if anchor not in declared_offsets:
                continue
            if finding["kind"] == "bcore":
                mapped_bcores.append(finding["metadata"])
            elif finding["kind"] == "eeprom":
                mapped_eeproms.append(finding["metadata"])
    if len(mapped_bcores) == 1:
        result["bcore"]["selected"] = mapped_bcores[0]
    if len(mapped_eeproms) == 1:
        result["eeprom"] = {
            **mapped_eeproms[0],
            "findings": eeprom_findings,
            "selected": mapped_eeproms[0],
        }
    return result
