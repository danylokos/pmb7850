from __future__ import annotations

import argparse
import json
import re
import struct
from pathlib import Path
from typing import Any

from ..layout.catalog import load_layout
from .mining import mine_fullflash
from ..firmware.xbi import FirmwareError, FlashLayout


def _bcd_byte(value: int) -> int | None:
    high, low = value >> 4, value & 0x0F
    return high * 10 + low if high <= 9 and low <= 9 else None


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


def _metadata_field(data: bytes, offset: int, pattern: str) -> str | None:
    field = data[offset:offset + 16]
    if len(field) != 16 or b"\0" not in field:
        return None
    value, padding = field.split(b"\0", 1)
    if not value or any(byte < 0x20 or byte > 0x7E for byte in value):
        return None
    if any(padding):
        return None
    text = value.decode("ascii")
    return text if re.fullmatch(pattern, text) else None


def _scan_firmware_metadata(data: bytes) -> list[dict[str, Any]]:
    fixed = bytes.fromhex("FF 0A 50 14 14 01 00 D5 21")
    instances: list[dict[str, Any]] = []
    for metadata_offset in range(0xFF50, len(data) - 0x90 + 1, 0x10000):
        metadata = data[metadata_offset:metadata_offset + 16]
        software = _bcd_byte(metadata[0]) if len(metadata) == 16 else None
        if (software is None or metadata[1:10] != fixed
                or metadata[12:16] != b"\xFF" * 4):
            continue
        langpack = _metadata_field(data, metadata_offset + 0x10,
                                   r"(?i:lg)[0-9]+")
        model = _metadata_field(data, metadata_offset + 0x20,
                                r"[A-Z][A-Z0-9]{1,14}")
        if (not langpack or not model
                or data[metadata_offset + 0x30:metadata_offset + 0x38]
                != b"SIEMENS\0"
                or metadata_offset < 0x7FF50):
            continue
        metadata_view_offset = metadata_offset - 0x7FF50
        tuple_offset = metadata_view_offset + 0x7FE26
        bcore_software_offset = metadata_view_offset + 0x32C
        if tuple_offset + 4 > len(data) or bcore_software_offset >= len(data):
            continue
        manufacturer, device = struct.unpack_from("<HH", data, tuple_offset)
        vendor, engine, classification = _FLASH_ENGINES.get(
            (manufacturer, device),
            ({
                0x0001: "AMD", 0x0020: "ST", 0x002C: "Micron",
                0x0089: "Intel",
            }.get(manufacturer, "unknown"), None, "unsupported"),
        )
        bcore_raw = data[bcore_software_offset]
        instances.append({
            "file_offset": f"0x{metadata_offset:X}",
            "metadata_view_offset": f"0x{metadata_view_offset:X}",
            "software_version": software,
            "software_version_raw": f"0x{metadata[0]:02X}",
            "langpack": langpack,
            "model": model,
            "bcore_software_version": _bcd_byte(bcore_raw),
            "bcore_software_version_raw": f"0x{bcore_raw:02X}",
            "flash": {
                "vendor": vendor,
                "manufacturer_id": f"0x{manufacturer:04X}",
                "device_id": f"0x{device:04X}",
                "raw": data[tuple_offset:tuple_offset + 4].hex(" "),
                "engine": engine,
                "classification": classification,
                "emulator_approximation": classification == "compatible",
            },
        })
    return instances


def _select_firmware_metadata(
        instances: list[dict[str, Any]]
) -> tuple[dict[str, Any] | None, bool]:
    if not instances:
        return None, False
    comparable = {
        key: value for key, value in instances[0].items()
        if key not in ("file_offset", "metadata_view_offset")
    }
    conflict = any(
        {key: value for key, value in instance.items()
         if key not in ("file_offset", "metadata_view_offset")} != comparable
        for instance in instances[1:]
    )
    selected = dict(instances[0])
    selected["instance_count"] = len(instances)
    return (None if conflict else selected), conflict


def _audit_metadata(data: bytes, layout: FlashLayout,
                    view: dict[str, Any]) -> dict[str, Any] | None:
    """Compatibility helper for assembly donor validation."""
    del layout, view
    selected, _conflict = _select_firmware_metadata(
        _scan_firmware_metadata(data)
    )
    return selected


def _layout_marker_specs(layout: FlashLayout) -> list[tuple[str, int, bytes]]:
    specs: list[tuple[str, int, bytes]] = []
    for region in layout.regions:
        if region.name == "T9":
            specs.append((region.name, region.offset, b"T9"))
        elif region.name == "LangPack":
            specs.append((region.name, region.offset, b"\xBB\xBB"))
        elif region.name in ("FFS(A)", "FFS_B", "FFS_C"):
            specs.extend((region.name, region.offset + delta, b"\xFE\xFEFFS")
                         for delta in (0x10, 0x80))
        elif region.name == "EE_FS":
            specs.extend((region.name, region.offset + delta, b"\xFE\xFEEE_FS")
                         for delta in (0x10, 0x80))
        elif region.name == "EEPROM":
            specs.extend((region.name, region.offset + delta, b"\xFE\xFEEELITE")
                         for delta in (0x10, 0x80))
    return specs


def _score_layout_view(data: bytes, layout: FlashLayout,
                       file_offset: int, layout_offset: int) -> int:
    score = 0
    for _name, expected, marker in _layout_marker_specs(layout):
        relative = expected - layout_offset
        if 0 <= relative <= len(data) - file_offset - len(marker):
            if data[file_offset + relative:file_offset + relative + len(marker)] == marker:
                score += 2
    metadata = 0x87FF50 - layout.base
    relative = metadata - layout_offset
    if 0 <= relative <= len(data) - file_offset - 4:
        block = data[file_offset + relative:file_offset + relative + 4]
        if block[1:4] == b"\xFF\x0A\x50" and _bcd_byte(block[0]) is not None:
            score += 5
    return score


def _choose_layout_view(data: bytes, layout: FlashLayout,
                        flash_address: int | None) -> dict[str, Any]:
    size = len(data)
    warnings: list[str] = []
    if flash_address is not None:
        layout_offset = flash_address - layout.base
        if layout_offset < 0 or layout_offset >= layout.length:
            raise FirmwareError(
                f"flash address 0x{flash_address:X} is outside layout {layout.name}"
            )
        covered = min(size, layout.length - layout_offset)
        if covered < size:
            warnings.append(
                f"{size - covered} input bytes extend beyond the layout"
            )
        return {
            "kind": "explicit",
            "confidence": "explicit",
            "file_offset": 0,
            "layout_offset": layout_offset,
            "length": covered,
            "warnings": warnings,
        }
    if size == layout.length:
        return {
            "kind": "exact",
            "confidence": "exact-size",
            "file_offset": 0,
            "layout_offset": 0,
            "length": size,
            "warnings": warnings,
        }
    if size > layout.length:
        repeated = (size % layout.length == 0
                    and all(data[offset:offset + layout.length] == data[:layout.length]
                            for offset in range(layout.length, size, layout.length)))
        candidates = {0, size - layout.length}
        for offset in range(0, size - layout.length + 1, 0x10000):
            candidates.add(offset)
        scored = [( _score_layout_view(data, layout, offset, 0), offset)
                  for offset in sorted(candidates)]
        best_score = max(score for score, _offset in scored)
        best = [offset for score, offset in scored if score == best_score]
        if len(best) > 1 and not repeated:
            return {
                "kind": "embedded-unresolved",
                "confidence": "unresolved",
                "file_offset": None,
                "layout_offset": None,
                "length": layout.length,
                "extra_input_bytes": size - layout.length,
                "marker_score": best_score,
                "warnings": [
                    f"multiple embedded layout views tie at score {best_score}; "
                    "pass --flash-address after extracting the intended raw view"
                ],
            }
        file_offset = best[0]
        return {
            "kind": "repeated" if repeated else "embedded-view",
            "confidence": "content-markers" if best_score else "size-only",
            "file_offset": file_offset,
            "layout_offset": 0,
            "length": layout.length,
            "extra_input_bytes": size - layout.length,
            "marker_score": best_score,
            "warnings": warnings,
        }

    candidates = {0, layout.length - size}
    candidates.update(region.offset for region in layout.regions
                      if region.offset <= layout.length - size)
    for offset in range(0, layout.length - size + 1, 0x10000):
        candidates.add(offset)
    scored = [(_score_layout_view(data, layout, 0, offset), offset)
              for offset in sorted(candidates)]
    best_score = max(score for score, _offset in scored)
    best = [offset for score, offset in scored if score == best_score]
    if best_score == 0 or len(best) != 1:
        return {
            "kind": "partial-unresolved",
            "confidence": "unresolved",
            "file_offset": None,
            "layout_offset": None,
            "length": size,
            "marker_score": best_score,
            "warnings": [
                "partial input cannot be aligned uniquely; pass --flash-address"
            ],
        }
    return {
        "kind": "partial",
        "confidence": "content-markers",
        "file_offset": 0,
        "layout_offset": best[0],
        "length": size,
        "marker_score": best_score,
        "warnings": warnings,
    }


def audit_fullflash(data: bytes, layout: FlashLayout | None = None,
                    flash_address: int | None = None,
                    file_offset: int = 0,
                    layout_file: Path | None = None) -> dict[str, Any]:
    """Statically audit a raw or proprietary fullflash candidate."""
    return mine_fullflash(
        data,
        layout=layout,
        flash_address=flash_address,
        file_offset=file_offset,
        layout_file=layout_file,
    )


def command_audit_dump(args: argparse.Namespace) -> None:
    if args.layout_file and not args.layout:
        raise FirmwareError("--layout-file requires --layout")
    layout = load_layout(args.layout, args.layout_file).layout if args.layout else None
    audit = audit_fullflash(
        args.input.read_bytes(),
        layout,
        args.flash_address,
        getattr(args, "file_offset", 0),
        args.layout_file,
    )
    audit["input"]["path"] = str(args.input)
    if args.json:
        print(json.dumps(audit, indent=2, sort_keys=True))
        return
    print(f"input        : {args.input}")
    print(f"type         : {audit['content_type']}")
    print(f"size         : {audit['size']}")
    print(f"sha256       : {audit['sha256']}")
    representation = audit["representation"]
    print(f"representation: {representation['kind']}")
    if representation.get("chip_order"):
        print(f"chip order    : {representation['chip_order']}")
    resolution = audit["layout_resolution"]
    if resolution["selected"]:
        selected = resolution["selected"]
        print(f"layout       : {selected['layout']} ({resolution['status']})")
        print(f"mapping      : layout = source + {selected['shift']:#x}")
    else:
        print(f"layout       : unresolved ({len(resolution['candidates'])} candidates)")
    metadata = audit.get("metadata")
    if metadata:
        bcore_software = metadata.get("bcore_software_version")
        print("metadata     : "
              f"{metadata.get('model') or '?'} SW{metadata['software_version']} "
              f"{metadata.get('langpack') or '?'}; "
              f"BCORE SW{bcore_software if bcore_software is not None else '?'}; "
              f"metadata view offset "
              f"{metadata['metadata_view_file_offset']}")
        flash = metadata.get("flash")
        if flash:
            approximation = (
                " emulator approximation"
                if flash["emulator_approximation"] else ""
            )
            print("flash        : "
                  f"{flash['vendor']} "
                  f"{flash['manufacturer_id']}/{flash['device_id']} -> "
                  f"{flash['engine'] or 'unsupported'} "
                  f"({flash['classification']}{approximation})")
    coverage = audit.get("coverage")
    if coverage and coverage["missing_ranges"]:
        missing = ", ".join(
            f"{item['from']}..{item['to_exclusive']}"
            for item in coverage["missing_ranges"]
        )
        print(f"missing      : {missing}")
    if audit.get("regions"):
        print("regions      :")
        for region in audit["regions"]:
            print(
                f"  {region['name']:<10} {region['state']:<10} "
                f"{region['coverage_percent']:6.2f}% "
                f"signature={region['signature_status']}"
            )
    reset_bytes = audit.get("reset", {}).get("bytes")
    if reset_bytes:
        print(f"reset vector: {reset_bytes}")
    bcore = audit.get("bcore", {}).get("selected")
    if bcore:
        print(
            f"bcore       : {bcore['model']} SW{bcore['software_version']} "
            f"header-file-offset={bcore['header_file_offset']} "
            f"header={bcore['structural_header_raw']}"
        )
    parsed_eeprom = audit.get("eeprom", {})
    if parsed_eeprom.get("status") == "parsed":
        imeis = parsed_eeprom["imei"]
        rendered = ", ".join(
            f"{block}={imeis[block]['imei'] or '?'}"
            for block in ("76", "5009")
        )
        block_ids = [block["id"] for block in parsed_eeprom["blocks"]]
        print(
            f"eeprom      : {parsed_eeprom['selected_block_count']} selected, "
            f"{parsed_eeprom['active_record_count']} active, "
            f"{parsed_eeprom['history_record_count']} history; {rendered}"
        )
        if getattr(args, "blocks", False):
            for block in parsed_eeprom["blocks"]:
                print(
                    f"  {block['id']:6d} {block['id_hex']} "
                    f"len={block['length']:5d} class={block['class']}"
                )
        else:
            shown = ", ".join(map(str, block_ids[:24]))
            suffix = (
                f", ... (+{len(block_ids) - 24})"
                if len(block_ids) > 24 else ""
            )
            print(f"blocks       : {shown}{suffix}")
    cold_boot = audit.get("cold_boot")
    if cold_boot:
        detail = ", ".join(cold_boot["reasons"]) or "reset and BCORE valid"
        print(f"cold boot    : {cold_boot['status']} ({detail})")
    for warning in audit["warnings"]:
        print(f"warning      : {warning}")
