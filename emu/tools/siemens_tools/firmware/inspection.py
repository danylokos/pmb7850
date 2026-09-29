from __future__ import annotations

import argparse
import io
import json
import os
import re
import sys
import tempfile
import zipfile
from pathlib import Path
from typing import Any

from .compression import convert_xbi_to_flash
from .containers import detect_exe_type, detect_service_exe_version, extract_exe
from .ffsinit import (
    discover_ffsinit_references,
    identify_ffsinit,
    is_ffsinit,
    recover_ffsinit_xfs,
    recover_ffsinit_bin_reference,
)
from .xbi import FirmwareError, XbiInfo, firmware_extension, is_xbi, parse_xbi


def detect_content_type(data: bytes) -> str:
    if is_xbi(data):
        return firmware_extension(parse_xbi(data))
    if data.startswith(b"PK\x03\x04"):
        return "zip"
    if data.startswith(b"[MapFileInfo]"):
        return "map"
    if data.startswith(b"MZ"):
        exe_type = detect_exe_type(data)
        if exe_type:
            payloads = extract_exe(data)
            if payloads:
                return f"{detect_content_type(payloads[0])}.exe"
        return "exe"
    return "bin"


def get_ffs_version(flash: bytes) -> str | None:
    non_sgold = "info.txt".encode("utf-16-be")
    start = 0
    pattern = re.compile(rb"([\w\d]+_\d+_[\w\d-]+_\d+_\d+)\n", re.I)
    while True:
        index = flash.find(non_sgold, start)
        if index < 0:
            break
        index += len(non_sgold)
        possible = bytes(byte for byte in flash[index:index + 256]
                         if byte != 0xFF)
        match = pattern.search(possible)
        if match:
            return match.group(1).decode("ascii")
        start = index + 1

    sgold_patterns = (
        "ccq_vinfo.txt".encode("utf-16-be"),
        b"ccq_vinfo.txt\0",
    )
    sgold_name = re.compile(rb"^[\w\d]+_\d+_[\w\d-]+_\d+_\d+$", re.I)
    for marker in sgold_patterns:
        start = 0
        while True:
            index = flash.find(marker, start)
            if index < 0:
                break
            index += len(marker)
            chars = bytearray()
            while index < len(flash) and flash[index] != 0x0A:
                byte = flash[index]
                if byte < 0x20 or byte > 0x7F:
                    chars.clear()
                    break
                chars.append(byte)
                index += 1
            if sgold_name.fullmatch(chars):
                return chars.decode("ascii")
            start = index + 1
    return None


def _map_payload_name(data: bytes) -> str | None:
    text = data.decode("utf-8", errors="replace")
    match = re.search(r"<([^>]+)>\s*$", text, re.S)
    return match.group(1).replace("_2D", "-") if match else None


def _zip_payload_name(data: bytes) -> str | None:
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            with archive.open("Config/ccq_vinfo.txt") as source:
                first = source.read().decode("utf-8", errors="replace").splitlines()
                return f"{first[0]}.zip" if first else None
    except (KeyError, OSError, zipfile.BadZipFile):
        return None


def _xbi_payload_name(data: bytes, info: XbiInfo) -> str | None:
    model = info.get("model")
    svn = info.get("svn")
    langpack = info.get("langpack")
    if not model or svn is None or not langpack:
        return None
    match = re.fullmatch(r"lg(\d+)", str(langpack), re.I)
    extension = firmware_extension(info)
    if not match:
        return f"{model}_{int(svn):02d}.{extension}"
    language = int(match.group(1))
    t9 = info.get("t9")
    if t9 is None:
        return f"{model}_{int(svn):02d}{language:02d}.{extension}"
    return f"{model}_{int(svn):02d}{language:02d}{int(t9):02d}.{extension}"


def _safe_name(name: str, fallback: str) -> str:
    name = name.replace("/", "_").replace("\\", "_").replace("\0", "")
    name = "".join(char for char in name if ord(char) >= 0x20).strip()
    return Path(name).name or fallback


def payload_name(input_path: Path, data: bytes, index: int) -> str:
    content_type = detect_content_type(data)
    fallback = f"{input_path.stem}.payload-{index}.{content_type}"
    name: str | None = None
    try:
        if content_type == "map":
            name = _map_payload_name(data)
        elif content_type == "zip":
            name = _zip_payload_name(data)
        elif is_xbi(data):
            info = parse_xbi(data)
            name = _xbi_payload_name(data, info)
            if content_type == "xfs":
                version = get_ffs_version(convert_xbi_to_flash(data, info))
                if version:
                    name = f"{version}.xfs"
    except FirmwareError:
        name = None
    return _safe_name(name or fallback, fallback)


def _json_value(value: Any) -> Any:
    if isinstance(value, bytes):
        return value.hex()
    if isinstance(value, dict):
        return {key: _json_value(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_json_value(item) for item in value]
    return value


def _describe_xbi(data: bytes) -> dict[str, Any]:
    info = parse_xbi(data)
    return {
        "kind": firmware_extension(info),
        "size": len(data),
        "xbi": {
            "format_version": info.format_version,
            "signed": info.signed,
            "signature_size": info.signature_size,
            "frame_stream_valid": True,
            "write_count": len(info.writes),
            **_json_value(info.fields),
        },
    }


def describe_bytes(data: bytes, name: str | None = None,
                   index: int | None = None) -> dict[str, Any]:
    if is_xbi(data):
        result = _describe_xbi(data)
    else:
        result = {"kind": detect_content_type(data), "size": len(data)}
    if name is not None:
        result["name"] = name
    if index is not None:
        result["index"] = index
    return result


def describe_path(input_path: Path) -> dict[str, Any]:
    data = input_path.read_bytes()
    exe_type = detect_exe_type(data)
    if exe_type:
        payloads = extract_exe(data)
        result: dict[str, Any] = {
            "path": str(input_path),
            "kind": "exe",
            "size": len(data),
            "exe_type": exe_type,
            "payloads": [],
        }
        if is_ffsinit(input_path, data):
            identity = identify_ffsinit(input_path, data)
            result["exe_type"] = "ffsinit"
            result["ffsinit"] = {
                "model": identity.model,
                "software_version": identity.software_version,
                "zip_sha256": identity.zip_sha256,
                "zip_size": identity.zip_size,
                "tree_sha256": identity.tree_sha256,
                "entry_order_sha256": identity.entry_order_sha256,
                "conversion": "exact-xfs-or-semantic-reference-bin",
            }
        if exe_type == "service":
            result["service_format_version"] = detect_service_exe_version(data)
        for index, payload in enumerate(payloads):
            name = payload_name(input_path, payload, index)
            result["payloads"].append(describe_bytes(payload, name, index))
        return result
    result = describe_bytes(data)
    result["path"] = str(input_path)
    return result


_ADDRESS_FIELDS = frozenset(("statistic_addr", "addr", "from", "to"))


def _format_scalar(name: str, value: Any) -> str:
    if isinstance(value, int) and name in _ADDRESS_FIELDS:
        return f"0x{value:06X}"
    if isinstance(value, bool):
        return "yes" if value else "no"
    return str(value)


def _format_nested(name: str, value: Any) -> str:
    if isinstance(value, dict):
        items = (
            f"{json.dumps(key)}:{_format_nested(key, item)}"
            for key, item in value.items()
        )
        return "{" + ",".join(items) + "}"
    if isinstance(value, (list, tuple)):
        return "[" + ",".join(
            _format_nested(name, item) for item in value
        ) + "]"
    if isinstance(value, int) and name in _ADDRESS_FIELDS:
        return _format_scalar(name, value)
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def _print_xbi_text(xbi: dict[str, Any], indent: str = "") -> None:
    for key, value in xbi.items():
        if isinstance(value, list):
            for index, item in enumerate(value):
                formatted = (
                    _format_nested(key, item)
                    if isinstance(item, (dict, list, tuple))
                    else _format_scalar(key, item)
                )
                print(f"{indent}{key}[{index}]: {formatted}")
        elif isinstance(value, dict):
            print(f"{indent}{key}: {_format_nested(key, value)}")
        else:
            print(f"{indent}{key}: {_format_scalar(key, value)}")


def print_description(description: dict[str, Any]) -> None:
    print(f"path: {description['path']}")
    print(f"kind: {description['kind']}")
    print(f"size: {description['size']}")
    if description["kind"] == "exe":
        print(f"exe_type: {description['exe_type']}")
        if "service_format_version" in description:
            print(f"service_format_version: "
                  f"{description['service_format_version']}")
        print(f"payload_count: {len(description['payloads'])}")
        for payload in description["payloads"]:
            print()
            print(f"payload[{payload['index']}]: {payload['name']}")
            print(f"  kind: {payload['kind']}")
            print(f"  size: {payload['size']}")
            if "xbi" in payload:
                _print_xbi_text(payload["xbi"], "  ")
    elif "xbi" in description:
        _print_xbi_text(description["xbi"])


def _atomic_write(output: Path, data: bytes, force: bool) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists() and not force:
        raise FirmwareError(f"output already exists: {output} (use --force)")
    fd, temp_name = tempfile.mkstemp(prefix=f".{output.name}.", suffix=".part",
                                     dir=output.parent)
    try:
        with os.fdopen(fd, "wb") as destination:
            destination.write(data)
            destination.flush()
            os.fsync(destination.fileno())
        if output.exists() and not force:
            raise FirmwareError(f"output already exists: {output} (use --force)")
        os.replace(temp_name, output)
    except BaseException:
        try:
            os.unlink(temp_name)
        except FileNotFoundError:
            pass
        raise


def _unique_payload_names(input_path: Path,
                          payloads: list[bytes]) -> list[str]:
    names: list[str] = []
    used: set[str] = set()
    for index, payload in enumerate(payloads):
        candidate = payload_name(input_path, payload, index)
        if candidate in used:
            item = Path(candidate)
            candidate = f"{item.stem}-{index}{item.suffix}"
        collision = 1
        while candidate in used:
            candidate = f"payload-{index}-{collision}.bin"
            collision += 1
        names.append(candidate)
        used.add(candidate)
    return names


def command_info(args: argparse.Namespace) -> None:
    description = describe_path(args.input)
    if args.json:
        print(json.dumps(description, indent=2, ensure_ascii=False))
    else:
        print_description(description)


def command_unpack(args: argparse.Namespace) -> None:
    data = args.input.read_bytes()
    if detect_exe_type(data) is None:
        raise FirmwareError("unpack expects a Siemens service/update executable")
    payloads = extract_exe(data)
    output_dir = args.output or Path(str(args.input.with_suffix("")) + ".unpacked")
    if output_dir.exists() and not args.force:
        raise FirmwareError(
            f"output directory already exists: {output_dir} (use --force)"
        )
    if output_dir.exists() and not output_dir.is_dir():
        raise FirmwareError(f"output path is not a directory: {output_dir}")
    output_dir.mkdir(parents=True, exist_ok=True)
    for name, payload in zip(_unique_payload_names(args.input, payloads), payloads):
        output = output_dir / name
        _atomic_write(output, payload, args.force)
        print(output)


def _select_convert_payload(input_path: Path, data: bytes,
                            selected_index: int | None) -> tuple[bytes, str]:
    if detect_exe_type(data) is None:
        if selected_index is not None:
            raise FirmwareError("--payload is only valid for executable inputs")
        if not is_xbi(data):
            raise FirmwareError("input is not an XBI-family firmware file")
        return data, input_path.with_suffix(".bin").name

    payloads = extract_exe(data)
    convertible = [index for index, payload in enumerate(payloads)
                   if is_xbi(payload)]
    if selected_index is not None:
        if selected_index < 0 or selected_index >= len(payloads):
            raise FirmwareError(
                f"payload index {selected_index} is out of range "
                f"(0..{len(payloads) - 1})"
            )
        if selected_index not in convertible:
            raise FirmwareError(f"payload {selected_index} is not XBI firmware")
        index = selected_index
    elif len(convertible) == 1:
        index = convertible[0]
    elif not convertible:
        raise FirmwareError("executable contains no XBI firmware payload")
    else:
        choices = ", ".join(str(index) for index in convertible)
        raise FirmwareError(
            f"executable contains multiple firmware payloads ({choices}); "
            "select one with --payload"
        )
    payload = payloads[index]
    name = payload_name(input_path, payload, index)
    return payload, str(Path(name).with_suffix(".bin"))


def command_convert(args: argparse.Namespace) -> None:
    data = args.input.read_bytes()
    if is_ffsinit(args.input, data):
        if args.payload is not None:
            raise FirmwareError("--payload is not valid for FFSInit inputs")
        roots = args.reference_root or [args.input.parent]
        references = discover_ffsinit_references(roots)
        recovery = None
        if args.bin:
            recovery = recover_ffsinit_bin_reference(
                args.input, data, references
            )
            reference = recovery.reference
        else:
            reference = recover_ffsinit_xfs(
                args.input, data, references
            )
        if args.bin:
            output = args.output or args.input.with_suffix(".bin")
            converted = convert_xbi_to_flash(reference.xfs)
            if recovery is not None and recovery.confidence == "heuristic":
                print(
                    "warning: BIN uses a canonical file-tree reference",
                    file=sys.stderr,
                )
        else:
            output = args.output or args.input.with_suffix(".xfs")
            converted = reference.xfs
        _atomic_write(output, converted, args.force)
        print(output)
        return
    if args.bin:
        raise FirmwareError("--bin is only valid for FFSInit executable inputs")
    if args.reference_root:
        raise FirmwareError("--reference-root is only valid for FFSInit inputs")
    payload, default_name = _select_convert_payload(
        args.input, data, args.payload
    )
    output = args.output or args.input.with_name(default_name)
    flash = convert_xbi_to_flash(payload)
    _atomic_write(output, flash, args.force)
    print(output)
