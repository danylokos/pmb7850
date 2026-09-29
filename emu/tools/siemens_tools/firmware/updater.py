from __future__ import annotations

import argparse
import hashlib
import json
import os
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .containers import detect_exe_type
from .xbi import FirmwareError
from .updater_analysis import analyze_updater_firmware


MANIFEST_SCHEMA = "siemens-mobile-updater-extraction"
MANIFEST_VERSION = 3
TRANSPORT_FILENAME = "mobile-updater.frames.bin"
IMAGE_FILENAME = "mobile-updater.c166.bin"
MANIFEST_FILENAME = "manifest.json"
IMAGE_FILL = 0xFF
_ACCESSOR_PREFIX = bytes.fromhex("8B 44 24 04 8B 0D")
_ACCESSOR_MIDDLE = bytes.fromhex("89 08 B8")
_ACCESSOR_SUFFIX = b"\xC3"


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _need(data: bytes, offset: int, size: int, description: str) -> None:
    if offset < 0 or size < 0 or offset + size > len(data):
        raise FirmwareError(f"truncated {description} at 0x{offset:x}")


def _u16le(data: bytes, offset: int) -> int:
    _need(data, offset, 2, "uint16")
    return int.from_bytes(data[offset:offset + 2], "little")


def _u32le(data: bytes, offset: int) -> int:
    _need(data, offset, 4, "uint32")
    return int.from_bytes(data[offset:offset + 4], "little")


@dataclass(frozen=True)
class PeSection:
    name: str
    virtual_address: int
    virtual_size: int
    raw_offset: int
    raw_size: int


@dataclass(frozen=True)
class PeImage:
    image_base: int
    sections: tuple[PeSection, ...]

    def va_to_offset(self, va: int, size: int = 1) -> int:
        rva = va - self.image_base
        for section in self.sections:
            relative = rva - section.virtual_address
            if relative < 0 or relative + size > section.raw_size:
                continue
            return section.raw_offset + relative
        raise FirmwareError(f"PE address 0x{va:08x} is not file-backed")


@dataclass(frozen=True)
class UpdaterFrame:
    index: int
    stream_offset: int
    address: int
    length: int
    payload: bytes
    checksum: int

    @property
    def stream_end(self) -> int:
        return self.stream_offset + 5 + self.length

    @property
    def address_end(self) -> int:
        return self.address + self.length


@dataclass(frozen=True)
class AddressRange:
    start: int
    end: int


@dataclass(frozen=True)
class UpdaterExtraction:
    source_offset: int
    accessor_offset: int
    stream_pointer_va: int
    length_pointer_va: int
    stream: bytes
    frames: tuple[UpdaterFrame, ...]
    image: bytes
    mapped_ranges: tuple[AddressRange, ...]
    mapped_byte_count: int
    entry_transfer: dict[str, Any] | None


def _parse_pe(data: bytes) -> PeImage:
    if data[:2] != b"MZ":
        raise FirmwareError("updater input is not a PE executable")
    pe_offset = _u32le(data, 0x3C)
    _need(data, pe_offset, 24, "PE header")
    if data[pe_offset:pe_offset + 4] != b"PE\0\0":
        raise FirmwareError("invalid PE signature")
    section_count = _u16le(data, pe_offset + 6)
    optional_size = _u16le(data, pe_offset + 20)
    optional_offset = pe_offset + 24
    _need(data, optional_offset, optional_size, "PE optional header")
    if _u16le(data, optional_offset) != 0x10B:
        raise FirmwareError("updater input is not a PE32 executable")
    image_base = _u32le(data, optional_offset + 28)
    section_offset = optional_offset + optional_size
    _need(data, section_offset, section_count * 40, "PE section table")
    sections = []
    for index in range(section_count):
        offset = section_offset + index * 40
        raw_name = data[offset:offset + 8].split(b"\0", 1)[0]
        name = raw_name.decode("ascii", errors="replace")
        section = PeSection(
            name=name,
            virtual_size=_u32le(data, offset + 8),
            virtual_address=_u32le(data, offset + 12),
            raw_size=_u32le(data, offset + 16),
            raw_offset=_u32le(data, offset + 20),
        )
        _need(data, section.raw_offset, section.raw_size, f"PE section {name!r}")
        sections.append(section)
    return PeImage(image_base, tuple(sections))


def _xor_checksum(data: bytes) -> int:
    value = 0
    for byte in data:
        value ^= byte
    return value


def parse_updater_frames(stream: bytes) -> tuple[UpdaterFrame, ...]:
    if not stream:
        raise FirmwareError("empty mobile updater stream")
    frames = []
    cursor = 0
    occupied: dict[int, int] = {}
    while cursor < len(stream):
        _need(stream, cursor, 5, "mobile updater frame")
        base = _u16le(stream, cursor)
        address = base + stream[cursor + 2]
        length = stream[cursor + 3]
        frame_size = 5 + length
        _need(stream, cursor, frame_size, "mobile updater frame")
        frame_bytes = stream[cursor:cursor + frame_size]
        if _xor_checksum(frame_bytes) != 0:
            expected = frame_bytes[-1]
            actual = _xor_checksum(frame_bytes[:-1])
            raise FirmwareError(
                f"invalid mobile updater frame checksum at 0x{cursor:x}: "
                f"0x{expected:02x} != 0x{actual:02x}"
            )
        payload = frame_bytes[4:-1]
        for relative in range(length):
            mapped = address + relative
            if mapped in occupied:
                raise FirmwareError(
                    f"overlapping mobile updater frames {occupied[mapped]} and "
                    f"{len(frames)} at address 0x{mapped:04x}"
                )
            occupied[mapped] = len(frames)
        frames.append(UpdaterFrame(
            index=len(frames),
            stream_offset=cursor,
            address=address,
            length=length,
            payload=payload,
            checksum=frame_bytes[-1],
        ))
        cursor += frame_size
    return tuple(frames)


def _mapped_ranges(frames: tuple[UpdaterFrame, ...]) -> tuple[AddressRange, ...]:
    addresses = sorted(
        address
        for frame in frames
        for address in range(frame.address, frame.address_end)
    )
    if not addresses:
        raise FirmwareError("mobile updater stream has no mapped payload bytes")
    ranges: list[AddressRange] = []
    start = addresses[0]
    end = start + 1
    for address in addresses[1:]:
        if address == end:
            end += 1
            continue
        ranges.append(AddressRange(start, end))
        start = address
        end = address + 1
    ranges.append(AddressRange(start, end))
    return tuple(ranges)


def _materialize_image(
    frames: tuple[UpdaterFrame, ...],
) -> tuple[bytes, tuple[AddressRange, ...], int]:
    ranges = _mapped_ranges(frames)
    image = bytearray([IMAGE_FILL]) * ranges[-1].end
    mapped = 0
    for frame in frames:
        image[frame.address:frame.address_end] = frame.payload
        mapped += frame.length
    return bytes(image), ranges, mapped


def _entry_transfer(image: bytes, frames: tuple[UpdaterFrame, ...]) -> dict[str, Any] | None:
    mapped = {
        address
        for frame in frames
        for address in range(frame.address, frame.address_end)
    }
    address = 0x0200
    if not all(address + offset in mapped for offset in range(4)):
        return None
    instruction = image[address:address + 4]
    if instruction[0] != 0xFA:
        return None
    segment = instruction[1]
    offset = int.from_bytes(instruction[2:4], "little")
    return {
        "kind": "jmps",
        "source": address,
        "target": (segment << 16) | offset,
        "segment": segment,
        "offset": offset,
        "bytes": instruction.hex(),
    }


def _candidate_accessors(data: bytes, pe: PeImage) -> list[tuple[int, int, int, int]]:
    candidates = []
    for section in pe.sections:
        if section.name != ".text":
            continue
        text = data[section.raw_offset:section.raw_offset + section.raw_size]
        cursor = 0
        while True:
            relative = text.find(_ACCESSOR_PREFIX, cursor)
            if relative < 0:
                break
            cursor = relative + 1
            if text[relative + 10:relative + 13] != _ACCESSOR_MIDDLE:
                continue
            if text[relative + 17:relative + 18] != _ACCESSOR_SUFFIX:
                continue
            accessor_offset = section.raw_offset + relative
            length_va = _u32le(text, relative + 6)
            stream_va = _u32le(text, relative + 13)
            try:
                length_offset = pe.va_to_offset(length_va, 4)
                length = _u32le(data, length_offset)
                stream_offset = pe.va_to_offset(stream_va, length)
                stream = data[stream_offset:stream_offset + length]
                parse_updater_frames(stream)
            except FirmwareError:
                continue
            candidates.append((accessor_offset, length_va, stream_va, stream_offset))
    return candidates


def parse_mobile_updater(data: bytes) -> UpdaterExtraction:
    if detect_exe_type(data) != "service":
        raise FirmwareError("updater expects a Siemens service executable")
    pe = _parse_pe(data)
    candidates = _candidate_accessors(data, pe)
    if not candidates:
        raise FirmwareError("cannot locate a valid mobile updater PE accessor")
    if len(candidates) != 1:
        raise FirmwareError(f"ambiguous mobile updater PE accessors: {len(candidates)}")
    accessor_offset, length_va, stream_va, stream_offset = candidates[0]
    length_offset = pe.va_to_offset(length_va, 4)
    length = _u32le(data, length_offset)
    stream = data[stream_offset:stream_offset + length]
    frames = parse_updater_frames(stream)
    image, ranges, mapped = _materialize_image(frames)
    return UpdaterExtraction(
        source_offset=stream_offset,
        accessor_offset=accessor_offset,
        stream_pointer_va=stream_va,
        length_pointer_va=length_va,
        stream=stream,
        frames=frames,
        image=image,
        mapped_ranges=ranges,
        mapped_byte_count=mapped,
        entry_transfer=_entry_transfer(image, frames),
    )


def _range_json(start: int, end: int) -> dict[str, int]:
    return {"start": start, "end": end}


def describe_mobile_updater(
    input_path: Path,
    data: bytes,
    extraction: UpdaterExtraction,
    *,
    include_frames: bool = False,
    compare_firmware: bool = False,
) -> dict[str, Any]:
    analysis = analyze_updater_firmware(data, extraction, compare_firmware)
    result: dict[str, Any] = {
        "schema": MANIFEST_SCHEMA,
        "schema_version": MANIFEST_VERSION,
        "input": {
            "path": str(input_path),
            "size": len(data),
            "sha256": _sha256(data),
        },
        "locator": {
            "method": "pe-accessor",
            "accessor_file_offset": extraction.accessor_offset,
            "stream_pointer_va": extraction.stream_pointer_va,
            "length_pointer_va": extraction.length_pointer_va,
        },
        "transport": {
            "source_range": _range_json(
                extraction.source_offset,
                extraction.source_offset + len(extraction.stream),
            ),
            "size": len(extraction.stream),
            "sha256": _sha256(extraction.stream),
            "frame_format": "base-le16,offset-u8,length-u8,payload,xor8",
            "frame_count": len(extraction.frames),
            "all_checksums_valid": True,
        },
        "image": {
            "base_address": 0,
            "size": len(extraction.image),
            "sha256": _sha256(extraction.image),
            "fill_byte": IMAGE_FILL,
            "mapped_byte_count": extraction.mapped_byte_count,
            "mapped_ranges": [
                _range_json(item.start, item.end)
                for item in extraction.mapped_ranges
            ],
            "entry_transfer": extraction.entry_transfer,
        },
    }
    result["firmware_payloads"] = analysis["payloads"]
    result["generation"] = analysis["generation"]
    result["semantics"] = analysis["semantics"]
    result["firmware_comparison"] = analysis["comparison"]
    if include_frames:
        result["transport"]["frames"] = [
            {
                "index": frame.index,
                "stream_range": _range_json(frame.stream_offset, frame.stream_end),
                "source_range": _range_json(
                    extraction.source_offset + frame.stream_offset,
                    extraction.source_offset + frame.stream_end,
                ),
                "address_range": _range_json(frame.address, frame.address_end),
                "length": frame.length,
                "checksum": frame.checksum,
            }
            for frame in extraction.frames
        ]
    return result


def _atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".part", dir=path.parent
    )
    try:
        with os.fdopen(descriptor, "wb") as output:
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


def _safe_owned_name(value: Any) -> str | None:
    if not isinstance(value, str):
        return None
    path = Path(value)
    if path.name != value or value in ("", ".", ".."):
        return None
    return value


def _old_owned_paths(output_dir: Path) -> set[Path]:
    manifest_path = output_dir / MANIFEST_FILENAME
    if not manifest_path.is_file():
        return set()
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return set()
    if manifest.get("schema") != MANIFEST_SCHEMA:
        return set()
    owned = {manifest_path}
    for artifact in manifest.get("artifacts", []):
        if not isinstance(artifact, dict):
            continue
        name = _safe_owned_name(artifact.get("path"))
        if name is not None:
            owned.add(output_dir / name)
    return owned


def write_mobile_updater(
    input_path: Path,
    output_dir: Path,
    data: bytes,
    extraction: UpdaterExtraction,
    force: bool,
) -> list[Path]:
    if output_dir.exists() and not output_dir.is_dir():
        raise FirmwareError(f"output path is not a directory: {output_dir}")
    if output_dir.exists() and not force:
        raise FirmwareError(
            f"output directory already exists: {output_dir} (use --force)"
        )
    old_owned = _old_owned_paths(output_dir) if output_dir.exists() else set()
    output_dir.mkdir(parents=True, exist_ok=True)
    transport_path = output_dir / TRANSPORT_FILENAME
    image_path = output_dir / IMAGE_FILENAME
    manifest_path = output_dir / MANIFEST_FILENAME
    manifest = describe_mobile_updater(
        input_path, data, extraction, include_frames=True
    )
    manifest["artifacts"] = [
        {
            "kind": "transport-frames",
            "path": TRANSPORT_FILENAME,
            "size": len(extraction.stream),
            "sha256": _sha256(extraction.stream),
        },
        {
            "kind": "address-preserving-c166-image",
            "path": IMAGE_FILENAME,
            "size": len(extraction.image),
            "sha256": _sha256(extraction.image),
        },
    ]
    new_owned = {transport_path, image_path, manifest_path}
    for stale in sorted(old_owned - new_owned):
        if stale.is_file() or stale.is_symlink():
            stale.unlink()
    _atomic_write(transport_path, extraction.stream)
    _atomic_write(image_path, extraction.image)
    encoded = (json.dumps(manifest, indent=2, ensure_ascii=False) + "\n").encode("utf-8")
    _atomic_write(manifest_path, encoded)
    return [transport_path, image_path, manifest_path]


def _format_address(value: int) -> str:
    return f"0x{value:06X}"


def print_mobile_updater(description: dict[str, Any]) -> None:
    source = description["transport"]["source_range"]
    image = description["image"]
    print(f"path: {description['input']['path']}")
    print(f"size: {description['input']['size']}")
    print(f"sha256: {description['input']['sha256']}")
    print(f"source_offset: 0x{source['start']:X}")
    print(f"source_end: 0x{source['end']:X}")
    print(f"transport_size: {description['transport']['size']}")
    print(f"transport_sha256: {description['transport']['sha256']}")
    print(f"frame_count: {description['transport']['frame_count']}")
    print(f"all_checksums_valid: yes")
    print(f"image_size: {image['size']}")
    print(f"image_sha256: {image['sha256']}")
    print(f"mapped_byte_count: {image['mapped_byte_count']}")
    print(f"mapped_range_count: {len(image['mapped_ranges'])}")
    transfer = image.get("entry_transfer")
    if transfer is not None:
        print(
            f"entry_jump: {_format_address(transfer['source'])} -> "
            f"{_format_address(transfer['target'])}"
        )
    generation = description["generation"]
    print(f"updater_profile: {generation['profile_id'] or 'unrecognized'}")
    for payload in description["firmware_payloads"]:
        index = payload.get("index")
        print(f"firmware_payload[{index}]_status: {payload['status']}")
        if payload.get("status") == "confirmed":
            print(f"firmware_payload[{index}]_kind: {payload['kind']}")
            print(f"firmware_payload[{index}]_size: {payload['size']}")
            print(f"firmware_payload[{index}]_sha256: {payload['sha256']}")
            print(f"firmware_payload[{index}]_model: {payload.get('model')}")
            print(
                f"firmware_payload[{index}]_software_version: "
                f"{payload.get('software_version')}"
            )
            print(f"firmware_payload[{index}]_cpu: {payload.get('cpu')}")
            print(f"firmware_payload[{index}]_flash_size: {payload.get('flash_size')}")
            statistic = payload.get("statistic_addr")
            if isinstance(statistic, int):
                print(f"firmware_payload[{index}]_statistic_addr: {_format_address(statistic)}")
            split_info = json.dumps(
                payload.get("split_info"), separators=(",", ":")
            )
            print(f"firmware_payload[{index}]_split_info: {split_info}")
        occurrences = payload.get("raw_updater_stream_occurrences", [])
        print(f"firmware_payload[{index}]_raw_stream_occurrences: {len(occurrences)}")
    semantics = description["semantics"]
    print(f"semantics_status: {semantics['status']}")
    finalization = semantics.get("statistics_finalization_write", {})
    finalization_status = finalization.get("status", "unrecognized")
    print(f"finalization_write_status: {finalization_status}")
    if finalization_status == "confirmed":
        destination = finalization["destination"]
        print(
            "finalization_write_address: "
            f"{_format_address(destination['address'])}"
        )
        print(
            "finalization_write_address_source: "
            f"{destination['source']['kind']}"
        )
        print(
            "finalization_write_constructor_address: "
            f"{_format_address(finalization['constructor_address'])}"
        )
        print(
            "finalization_write_callsite_address: "
            f"{_format_address(finalization['constructor_callsite_address'])}"
        )
        print(
            "finalization_write_flash_writer_address: "
            f"{_format_address(finalization['flash_writer_address'])}"
        )
        print(f"finalization_write_bytes: {finalization['write']['bytes']}")
        decoded = finalization["write"]["decoded_instruction"]
        print(
            "finalization_write_decoded_target: "
            f"{_format_address(decoded['target'])}"
        )
        print(f"finalization_write_guard: {finalization['guard']['condition']}")
    statistics = semantics.get("statistics", {})
    print(f"statistics_status: {statistics.get('status', 'unrecognized')}")
    if statistics.get("status") == "confirmed":
        checksum = statistics["checksum"]
        print(f"statistics_checksum: {checksum['algorithm']}")
        print(
            "statistics_checksum_offsets: "
            f"0x{checksum['xor16_offset']:X},"
            f"0x{checksum['sum16_offset']:X}"
        )
        print(
            "statistics_range_table_offset: "
            f"0x{checksum['range_table_offset']:X}"
        )
        body = statistics["mutable_body"]
        print(
            "statistics_mutable_span: "
            f"[0x{body['start_offset']:X},0x{body['end_offset']:X})"
        )
        success = statistics["success_response"]
        print(
            "statistics_success_span: "
            f"[0x{success['start_offset']:X},"
            f"0x{success['end_offset']:X})"
        )
        error = statistics["error_response"]["code"]
        print(f"statistics_error_response: 0x{error:02X}")
    relocated = description["firmware_comparison"]["relocated_image"]
    print(f"relocated_image_status: {relocated['status']}")


def command_updater_info(args: argparse.Namespace) -> None:
    data = args.input.read_bytes()
    extraction = parse_mobile_updater(data)
    description = describe_mobile_updater(
        args.input, data, extraction,
        compare_firmware=getattr(args, "compare_firmware", False),
    )
    if args.json:
        print(json.dumps(description, indent=2, ensure_ascii=False))
    else:
        print_mobile_updater(description)


def command_updater_extract(args: argparse.Namespace) -> None:
    data = args.input.read_bytes()
    extraction = parse_mobile_updater(data)
    output_dir = args.output or Path(str(args.input.with_suffix("")) + ".updater")
    for path in write_mobile_updater(
        args.input, output_dir, data, extraction, args.force
    ):
        print(path)
