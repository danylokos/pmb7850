#!/usr/bin/env python3
"""Capture the live Siemens x55 ``*#06#`` information pages through UI v13.

The driver boots source bytes, injects only the external flash identity requested
by the manifest, uses sampled key releases, and records action-indexed raw RGB
hashes.  It intentionally contains no OCR or firmware-specific screen patching.
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import hashlib
import json
import signal
import struct
import subprocess
import tempfile
import time
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from tools.siemens_tools.bitmaps.raster import write_png
from tools.siemens_tools.eeprom.identity import verify_checks
from tools.siemens_tools.eeprom.storage import (
    extract_block,
    find_eeprom_region,
    load_eeprom_source,
    serialize_block,
)
from tools.cemu_trace.eeprom import create_report as create_eeprom_trace_report
from tools.cemu_trace.eeprom import format_report as format_eeprom_trace_report
from tools.cemu_trace.parquet import open_parquet

from tools import ui_protocol
MAGIC = ui_protocol.MAGIC
VERSION = ui_protocol.VERSION
HEADER = ui_protocol.HEADER
HELLO_PREFIX = struct.Struct("<HHHH")
STATS_PAYLOAD = ui_protocol.STATS_PAYLOAD
HELLO, FRAME, STATS, KEY, RELEASE_ALL, KEY_RELEASE_AFTER_SAMPLE, ERROR = range(1, 8)
ASC0_TX = 11
PAGE_NAMES = ("imei", "status", "cc-monitor")
TRACE_SELECTORS = "eeprom,keypad,lcd,lifecycle,serial"
SCHEMA = "x55-live-info-menu-capture"
SCHEMA_VERSION = 1
METADATA_HEADER = bytes((0xFF, 0x0A, 0x50, 0x14, 0x14, 0x01, 0x00, 0xD5, 0x21))


class CaptureError(RuntimeError):
    pass


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def _bcd(raw: int) -> int | None:
    high, low = raw >> 4, raw & 0x0f
    return high * 10 + low if high <= 9 and low <= 9 else None


def firmware_metadata(data: bytes) -> list[dict[str, Any]]:
    """Mirror the immutable image metadata fields selected by src/product.c."""
    records = []
    for offset in range(0xFF50, len(data) - 0x90 + 1, 0x10000):
        record = data[offset:offset + 0x90]
        version = _bcd(record[0])
        if (record[1:10] != METADATA_HEADER or version is None or
                record[12:16] != b"\xff" * 4 or offset < 0x7FF50):
            continue
        try:
            langpack = record[0x10:0x20].split(b"\0", 1)[0].decode("ascii")
            model = record[0x20:0x30].split(b"\0", 1)[0].decode("ascii")
        except UnicodeDecodeError:
            continue
        if not langpack.lower().startswith("lg") or not model or \
                record[0x30:0x38] != b"SIEMENS\0":
            continue
        view = offset - 0x7FF50
        if view + 0x7FE2A > len(data):
            continue
        manufacturer, device = struct.unpack_from("<HH", data, view + 0x7FE26)
        raw_bcore = data[view + 0x32C]
        records.append({
            "metadata_offset": offset, "metadata_view_offset": view,
            "model": model, "software_version": version,
            "software_version_raw": record[0], "langpack": langpack,
            "bcore_software_version": _bcd(raw_bcore),
            "bcore_software_version_raw": raw_bcore,
            "flash_manufacturer_id": manufacturer, "flash_device_id": device,
        })
    return records


def rgb_pixels(frame: bytes) -> list[tuple[int, int, int, int]]:
    return [(frame[i], frame[i + 1], frame[i + 2], 255)
            for i in range(0, len(frame), 3)]


def save_frame(directory: Path, stem: str, frame: bytes,
               width: int, height: int) -> dict[str, Any]:
    directory.mkdir(parents=True, exist_ok=True)
    native = directory / f"{stem}-native.png"
    scaled = directory / f"{stem}-4x.png"
    pixels = rgb_pixels(frame)
    write_png(native, pixels, width, height)
    write_png(scaled, pixels, width, height, scale=4)
    return {
        "raw_rgb_sha256": sha256_bytes(frame),
        "native_png": native.name,
        "native_png_sha256": sha256_file(native),
        "scaled_png": scaled.name,
        "scaled_png_sha256": sha256_file(scaled),
        "width": width,
        "height": height,
    }


def encode_packet(kind: int, payload: bytes = b"", sequence: int = 0,
                  run_id: bytes = bytes(16)) -> bytes:
    return ui_protocol.encode_packet(kind, payload, sequence=sequence, run_id=run_id)


async def read_packet(reader: asyncio.StreamReader):
    return await ui_protocol.read_packet(reader)


def decode_hello(payload: bytes) -> tuple[int, int, str, tuple[str, ...]]:
    try:
        return ui_protocol.decode_hello(payload)[:4]
    except ui_protocol.ProtocolError as exc:
        raise CaptureError(f"UI protocol error: {exc}") from exc


@dataclass
class UiState:
    width: int = 0
    height: int = 0
    model: str = ""
    keys: tuple[str, ...] = ()
    icount: int = 0
    ticks: int = 0
    pc: int = 0
    frame: bytes | None = None
    frame_icount: int = 0


class UiCapture:
    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        self.reader = reader
        self.writer = writer
        self.state = UiState()
        self.serial = bytearray()
        self.protocol_errors: list[str] = []
        self.sequence = 1
        self.run_id = None
        self.serial_mirror = ui_protocol.SerialMirror()
        self.lifecycle = None
        self.frames: list[dict[str, Any]] = []
        self.actions: list[dict[str, Any]] = []

    async def receive(self, timeout: float = 10.0) -> int:
        try:
            return await self._receive(timeout)
        except ui_protocol.ProtocolError as exc:
            raise CaptureError(f"UI protocol error: {exc}") from exc

    async def _receive(self, timeout: float) -> int:
        try:
            packet = await asyncio.wait_for(
                read_packet(self.reader), timeout)
        except (asyncio.IncompleteReadError, ConnectionError, OSError) as exc:
            raise CaptureError(f"UI socket closed: {exc}") from exc
        kind, payload, sequence, icount = packet.message_type, packet.payload, packet.sequence, packet.icount
        if self.run_id is None:
            if kind != HELLO:
                raise CaptureError("expected HELLO")
            self.run_id = packet.run_id
        elif packet.run_id != self.run_id:
            raise CaptureError("runtime identity changed on connection")
        self.state.icount = max(self.state.icount, icount)
        if kind == HELLO:
            (self.state.width, self.state.height,
             self.state.model, self.state.keys) = decode_hello(payload)
        elif kind == FRAME:
            expected = self.state.width * self.state.height * 3
            if not expected or len(payload) != expected:
                raise CaptureError("invalid FRAME size")
            self.state.frame = payload
            self.state.frame_icount = icount
            self.frames.append({"sequence": sequence, "icount": icount,
                                "raw_rgb_sha256": sha256_bytes(payload)})
        elif kind == STATS:
            if len(payload) != STATS_PAYLOAD.size:
                raise CaptureError("invalid STATS size")
            _elapsed, self.state.ticks, _guest, self.state.pc = ui_protocol.decode_stats(payload)[:4]
        elif kind == ui_protocol.LIFECYCLE:
            self.lifecycle = ui_protocol.decode_lifecycle(payload)
        elif kind == ui_protocol.ASC0_SUBSCRIBED:
            self.serial_mirror.subscribe(payload)
        elif kind == ASC0_TX:
            self.serial.extend(self.serial_mirror.live(payload))
        elif kind == ERROR:
            message = payload.decode("utf-8", "replace")
            self.protocol_errors.append(message)
            raise CaptureError(f"UI protocol error: {message}")
        return kind

    async def send(self, kind: int, payload: bytes = b"") -> None:
        self.writer.write(encode_packet(kind, payload, self.sequence, self.run_id or bytes(16)))
        self.sequence += 1
        await self.writer.drain()

    async def wait_until(self, predicate, *, timeout: float = 30.0) -> None:
        deadline = time.monotonic() + timeout
        while not predicate():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise CaptureError("wall-clock timeout waiting for UI state")
            await self.receive(min(remaining, 5.0))

    async def wait_ticks(self, start: int, delta: int,
                         *, timeout: float = 30.0) -> None:
        await self.wait_until(lambda: self.state.icount >= start + delta,
                              timeout=timeout)

    async def tap(self, name: str, settle_ticks: int = 2_500_000) -> dict[str, Any]:
        if name not in self.state.keys:
            raise CaptureError(f"key {name!r} absent from HELLO")
        index = self.state.keys.index(name)
        before = sha256_bytes(self.state.frame) if self.state.frame else None
        start = self.state.icount
        await self.send(KEY, bytes((index, 1)))
        await self.send(KEY_RELEASE_AFTER_SAMPLE, bytes((index,)))
        await self.wait_ticks(start, settle_ticks)
        after = sha256_bytes(self.state.frame) if self.state.frame else None
        action = {
            "index": len(self.actions), "key": name,
            "down_icount": start, "settled_icount": self.state.icount,
            "release": "after-sample", "before_hash": before,
            "after_hash": after,
        }
        self.actions.append(action)
        return action


def is_nonblank(frame: bytes | None) -> bool:
    if not frame:
        return False
    first = frame[:3]
    return any(frame[i:i + 3] != first for i in range(3, len(frame), 3))


def scroll_fixed(before_hash: str, after_frame: bytes | None) -> bool:
    return before_hash == sha256_bytes(after_frame or b"")


def pages_through(stop_after: str) -> tuple[str, ...]:
    """Return the inclusive page sequence, rejecting unknown stop points."""
    try:
        return PAGE_NAMES[:PAGE_NAMES.index(stop_after) + 1]
    except ValueError as exc:
        raise CaptureError(f"unknown information page {stop_after!r}") from exc


async def connect_socket(path: Path, process: asyncio.subprocess.Process,
                         timeout: float = 10.0) -> tuple[asyncio.StreamReader, asyncio.StreamWriter]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.returncode is not None:
            raise CaptureError(f"emulator exited before UI connect ({process.returncode})")
        try:
            return await asyncio.open_unix_connection(str(path))
        except (FileNotFoundError, ConnectionRefusedError, OSError):
            await asyncio.sleep(0.01)
    raise CaptureError("UI socket did not appear")


def command_for(entry: dict[str, Any], run_dir: Path, socket_path: Path,
                limit: int, emu_dir: Path) -> list[str]:
    source = Path(entry["source_path"])
    if not source.is_absolute():
        source = emu_dir.parent / source
    label = str(run_dir.relative_to(emu_dir / "shots"))
    command = [str(emu_dir / "bin/cemu_inst"), str(source),
               "--device", entry["device"]]
    if entry["identity_mode"] == "native-fsn":
        command += ["--fsn", entry["fsn"]]
        if entry["device"] == "m55":
            command += ["--imei", entry["imei"]]
    command += ["--limit", str(limit), "--batch-idle", "--ui-socket", str(socket_path),
               "--label", label, f"--trace={TRACE_SELECTORS}",
               "--dump-flash", "--benchmark-json"]
    return command


async def _read_stream(stream: asyncio.StreamReader) -> bytes:
    return await stream.read()


async def capture_run(entry: dict[str, Any], output: Path, run_name: str,
                      limit: int, emu_dir: Path,
                      stop_after: str = PAGE_NAMES[-1]) -> dict[str, Any]:
    run_dir = output / entry["slug"] / run_name
    frames_dir = run_dir / "frames"
    run_dir.mkdir(parents=True, exist_ok=True)
    socket_path = Path(tempfile.gettempdir()) / f"x55-info-{entry['slug']}-{run_name}.sock"
    with contextlib.suppress(FileNotFoundError):
        socket_path.unlink()
    command = command_for(entry, run_dir, socket_path, limit, emu_dir)
    started = time.monotonic()
    process = await asyncio.create_subprocess_exec(
        *command, cwd=emu_dir, stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE)
    stdout_task = asyncio.create_task(_read_stream(process.stdout))
    stderr_task = asyncio.create_task(_read_stream(process.stderr))
    ui: UiCapture | None = None
    captured: list[dict[str, Any]] = []
    classification = "timeout"
    reason = "tick limit reached before capture"
    try:
        reader, writer = await connect_socket(socket_path, process)
        ui = UiCapture(reader, writer)
        await ui.wait_until(lambda: bool(ui.state.keys and ui.state.frame))
        stable_start = ui.state.frame_icount
        stable_hash = sha256_bytes(ui.state.frame or b"")

        def ready() -> bool:
            nonlocal stable_start, stable_hash
            current = sha256_bytes(ui.state.frame or b"")
            if current != stable_hash:
                stable_hash, stable_start = current, ui.state.icount
            ready_floor = 40_000_000 if entry["device"] == "m55" else 50_000_000
            return (ui.state.icount >= ready_floor and is_nonblank(ui.state.frame) and
                    ui.state.icount - stable_start >= 750_000)

        # A 400M-tick retry can take several minutes on a heavily traced build.
        # The emulated tick bound, rather than a short host-time watchdog, is the
        # experiment's terminal condition.
        await ui.wait_until(ready, timeout=1800.0)
        ready_hash = sha256_bytes(ui.state.frame or b"")
        settle_ticks = 5_000_000 if entry["device"] == "m55" else 2_500_000
        code_actions = []
        for key in ("star", "hash", "0", "6", "hash"):
            code_actions.append(await ui.tap(key, settle_ticks))
        await ui.wait_ticks(ui.state.icount, settle_ticks)
        if any(action["before_hash"] == action["after_hash"]
               for action in code_actions):
            raise CaptureError("information code input was not reflected by the keypad UI")
        if sha256_bytes(ui.state.frame or b"") == ready_hash:
            raise CaptureError("information code produced no screen change")

        selected_pages = pages_through(stop_after)
        for page_index, page_name in enumerate(selected_pages, 1):
            if ui.state.frame is None:
                raise CaptureError("no frame for information page")
            position = 1
            meta = save_frame(frames_dir,
                              f"{page_index:02d}-{page_name}-{position:02d}",
                              ui.state.frame, ui.state.width, ui.state.height)
            meta.update({"page": page_name, "position": position,
                         "action_index": len(ui.actions) - 1,
                         "icount": ui.state.icount})
            captured.append(meta)
            for _ in range(15):
                before = sha256_bytes(ui.state.frame)
                action = await ui.tap("down", settle_ticks)
                action["scroll_terminated"] = scroll_fixed(before, ui.state.frame)
                if action["scroll_terminated"]:
                    break
                position += 1
                meta = save_frame(
                    frames_dir, f"{page_index:02d}-{page_name}-{position:02d}",
                    ui.state.frame or b"", ui.state.width, ui.state.height)
                meta.update({"page": page_name, "position": position,
                             "action_index": action["index"],
                             "icount": ui.state.icount})
                captured.append(meta)
            else:
                raise CaptureError(f"{page_name} did not reach a downward scroll fixed point")
            if page_index != len(selected_pages):
                before = sha256_bytes(ui.state.frame)
                await ui.tap("soft-left", settle_ticks)
                if sha256_bytes(ui.state.frame or b"") == before:
                    raise CaptureError(f"soft-left did not advance from {page_name}")
        classification = "pass"
        reason = f"page families captured through {stop_after}"
    except CaptureError as exc:
        reason = str(exc)
        if ui and b"EXIT:" in ui.serial:
            classification = "serial-exit"
        elif "identity-invalid" in reason.lower() or "applies only" in reason.lower():
            classification = "identity-invalid"
        elif ui and ui.state.frame and ui.state.icount >= limit - 100_000:
            classification = "timeout"
        else:
            classification = "unreachable"
    finally:
        if ui:
            with contextlib.suppress(Exception):
                await ui.send(RELEASE_ALL)
            ui.writer.close()
            with contextlib.suppress(Exception):
                await ui.writer.wait_closed()
        if process.returncode is None:
            process.send_signal(signal.SIGINT)
        with contextlib.suppress(asyncio.TimeoutError):
            await asyncio.wait_for(process.wait(), 30.0)
        if process.returncode is None:
            process.kill()
            await process.wait()
        stdout, stderr = await asyncio.gather(stdout_task, stderr_task)
        with contextlib.suppress(FileNotFoundError):
            socket_path.unlink()

    console = b"=== stdout ===\n" + stdout + b"\n=== stderr ===\n" + stderr
    (run_dir / "console.txt").write_bytes(console)
    serial = bytes(ui.serial) if ui else b""
    (run_dir / "serial.bin").write_bytes(serial)
    result = {
        "schema": SCHEMA, "schema_version": SCHEMA_VERSION,
        "image": entry["slug"], "run": run_name,
        "stop_after": stop_after,
        "classification": classification, "reason": reason,
        "command": command, "limit": limit,
        "elapsed_seconds": round(time.monotonic() - started, 6),
        "terminal": {
            "returncode": process.returncode,
            "icount": ui.state.icount if ui else 0,
            "ticks": ui.state.ticks if ui else 0,
            "pc": ui.state.pc if ui else 0,
            "serial_exit": b"EXIT:" in serial,
            "ui_protocol_errors": ui.protocol_errors if ui else [],
        },
        "keypad": list(ui.state.keys) if ui else [],
        "actions": ui.actions if ui else [],
        "ordered_frames": ui.frames if ui else [],
        "captures": captured,
        "serial_sha256": sha256_bytes(serial),
    }
    trace = run_dir / "trace/trace.parquet"
    if trace.exists():
        try:
            eeprom_report = create_eeprom_trace_report([trace])
            write_json(run_dir / "eeprom-trace.json", eeprom_report)
            (run_dir / "eeprom-trace.txt").write_text(
                format_eeprom_trace_report(eeprom_report), encoding="utf-8")
        except Exception as exc:
            result["eeprom_trace_error"] = str(exc)
        try:
            result["trace_validation"] = validate_action_trace(trace, result["actions"])
        except Exception as exc:
            result["trace_validation"] = {
                "error": str(exc), "matching_down_up_pairs": False}
    else:
        result["trace_validation"] = {
            "error": "trace absent", "matching_down_up_pairs": False}
    (run_dir / "serial.txt").write_text(
        "".join(chr(byte) if 0x20 <= byte <= 0x7e else f"\\x{byte:02X}"
                for byte in serial), encoding="ascii")
    write_json(run_dir / "capture.json", result)
    return result


def inventory_report(path: Path) -> dict[str, Any]:
    kind, blocks = load_eeprom_source(path)
    return {
        "source": str(path), "kind": kind,
        "valid": bool(blocks), "count": len(blocks),
        "blocks": [{**serialize_block(block),
                    "payload_sha256": sha256_bytes(block.payload)}
                   for _, block in sorted(blocks.items())],
    }


def inventory_diff(before: dict[str, Any], after: dict[str, Any]) -> dict[str, Any]:
    left = {item["id"]: item for item in before["blocks"]}
    right = {item["id"]: item for item in after["blocks"]}
    return {
        "added": sorted(set(right) - set(left)),
        "removed": sorted(set(left) - set(right)),
        "changed": sorted(key for key in set(left) & set(right)
                          if left[key]["payload_sha256"] != right[key]["payload_sha256"] or
                          left[key]["length"] != right[key]["length"]),
        "unchanged": sorted(key for key in set(left) & set(right)
                            if left[key]["payload_sha256"] == right[key]["payload_sha256"] and
                            left[key]["length"] == right[key]["length"]),
    }


def validate_action_trace(trace: Path, actions: list[dict[str, Any]]) -> dict[str, Any]:
    metadata = json.loads((trace / "manifest.json").read_text(encoding="utf-8"))
    kinds = {partition["kind"] for partition in metadata["partitions"]}
    if "keypad_input" not in kinds:
        return {
            "expected_transitions": len(actions) * 2,
            "actual_transitions": 0,
            "matching_down_up_pairs": not actions,
            "transitions": [],
        }
    connection = open_parquet(trace)
    try:
        rows = connection.execute(
            "SELECT info_button_str, info_pressed_bool, icount FROM trace "
            "WHERE kind='keypad_input' ORDER BY seq"
        ).fetchall()
    finally:
        connection.close()
    expected = [(action["key"], state) for action in actions
                for state in (True, False)]
    actual = [(button, pressed) for button, pressed, _icount in rows]
    return {
        "expected_transitions": len(expected),
        "actual_transitions": len(actual),
        "matching_down_up_pairs": actual == expected,
        "transitions": [{"key": button, "pressed": pressed, "icount": icount}
                        for button, pressed, icount in rows],
    }


def decode_identity(path: Path) -> tuple[str, dict[int, Any]]:
    _kind, blocks = load_eeprom_source(path)
    imei5009 = extract_block(blocks, 5009).get("imei")
    imei76 = extract_block(blocks, 76).get("imei")
    if not imei5009 or imei5009 != imei76:
        raise CaptureError(f"identity-invalid: 5009={imei5009!r}, 76={imei76!r}")
    return str(imei5009), blocks


def verify_fsn(blocks: dict[int, Any], fsn: int, imei: str) -> bool:
    dec5008 = bytes.fromhex(extract_block(blocks, 5008, fsn, imei)["plaintext_hex"])
    dec5077 = bytes.fromhex(extract_block(blocks, 5077, fsn, imei)["plaintext_hex"])
    checks = verify_checks(dec5008, dec5077)
    return not any(dec5008[offset] != value for offset, value in
                   ((30, checks[0][1]), (31, checks[0][2]),
                    (216, checks[1][1]), (217, checks[1][2]))) and \
           dec5077[224:226] == bytes(checks[2][1:])


def discover(repo: Path) -> list[dict[str, Any]]:
    images: list[dict[str, Any]] = []
    for device in ("c55", "m55"):
        recipes = repo / "fw/corpus" / device.upper() / "community/fullflashes"
        for recipe in sorted(recipes.glob("*.json")):
            obj = json.loads(recipe.read_text(encoding="utf-8"))
            images.append({
                "slug": f"{device}-community-{recipe.stem}", "device": device,
                "kind": "community", "recipe": str(recipe.relative_to(repo)),
                "source_path": obj["fullflash"]["source_path"],
                "source_sha256": obj["fullflash"]["sha256"],
                "source_size": obj["fullflash"]["size"],
            })
    for device in ("c55", "m55"):
        path = repo / "emu/shots" / f"{device}-eeprom-minimization-100m/final" / \
               f"{device}sw{'24' if device == 'c55' else '91'}-factory-eeprom-minimal.bin"
        images.append({
            "slug": f"{device}-minimized", "device": device, "kind": "minimized",
            "recipe": None, "source_path": str(path.relative_to(repo)),
            "source_sha256": sha256_file(path), "source_size": path.stat().st_size,
        })
    return images


def build_manifest(repo: Path, cache_path: Path | None = None) -> dict[str, Any]:
    cache = json.loads(cache_path.read_text()) if cache_path and cache_path.exists() else {}
    images = discover(repo)
    for index, entry in enumerate(images, 1):
        path = repo / entry["source_path"]
        if sha256_file(path) != entry["source_sha256"]:
            raise CaptureError(f"source hash mismatch: {path}")
        source_bytes = path.read_bytes()
        imei, blocks = decode_identity(path)
        region = find_eeprom_region(source_bytes)
        raw = source_bytes[region.region_file_base:region.region_file_base + region.size]
        eeprom_hash = sha256_bytes(raw)
        fsn = cache.get(imei)
        if fsn and not verify_fsn(blocks, int(fsn, 16), imei):
            fsn = None
        checks = None
        identity_mode = "built-in-eeprom"
        if fsn:
            value = int(fsn, 16)
            dec5008 = bytes.fromhex(
                extract_block(blocks, 5008, value, imei)["plaintext_hex"])
            dec5077 = bytes.fromhex(
                extract_block(blocks, 5077, value, imei)["plaintext_hex"])
            checks = verify_checks(dec5008, dec5077)
            identity_mode = "native-fsn"
        entry.update({"index": index, "imei": imei, "fsn": fsn,
                      "identity_mode": identity_mode,
                      "firmware_metadata": firmware_metadata(source_bytes),
                      "eeprom_sha256": eeprom_hash,
                      "protected_checks": (
                          [{"region": n, "sum": s, "xor": x}
                           for n, s, x in checks] if checks else None)})
        identity_args = (["--fsn", fsn] +
                         (["--imei", imei] if entry["device"] == "m55" else [])) \
                        if fsn else []
        entry["command_template"] = [
            "emu/bin/cemu_inst", entry["source_path"], "--device", entry["device"],
            *identity_args, "--limit", "200000000", "--batch-idle",
            "--ui-socket", "<socket>", "--label",
            f"x55-info-menu-live/{entry['slug']}/run-1",
            f"--trace={TRACE_SELECTORS}", "--dump-flash", "--benchmark-json",
        ]
    cemu = repo / "emu/bin/cemu_inst"
    return {
        "schema": "x55-live-info-menu-source-manifest", "schema_version": 1,
        "policy": {"startup": "fresh", "sim": "absent",
                   "overlay": "built-in identity records when native FSN is unknown",
                   "firmware_patches": [], "synthetic_behaviors": [],
                   "initial_limit": 200_000_000, "extended_limit": 400_000_000,
                   "identity": "native FSN when verified; built-in EEPROM identity otherwise"},
        "emulator": {"git_revision": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip(),
            "binary": str(cemu.relative_to(repo)), "binary_sha256": sha256_file(cemu)},
        "images": images,
    }


def replay_signature(result: dict[str, Any]) -> list[tuple[int, str, str]]:
    return [(item["action_index"], item["page"], item["raw_rgb_sha256"])
            for item in result["captures"]]


async def run_matrix(manifest: dict[str, Any], output: Path,
                     emu_dir: Path, only: set[str],
                     stop_after: str = PAGE_NAMES[-1]) -> list[dict[str, Any]]:
    matrix: list[dict[str, Any]] = []
    for entry in manifest["images"]:
        if only and entry["slug"] not in only:
            continue
        source = emu_dir.parent / entry["source_path"]
        image_dir = output / entry["slug"]
        before = inventory_report(source)
        write_json(image_dir / "eeprom-before.json", before)
        first = await capture_run(
            entry, output, "run-1", 200_000_000, emu_dir, stop_after)
        if first["classification"] == "timeout" and first["terminal"]["icount"] > 0:
            first = await capture_run(
                entry, output, "run-1-extended", 400_000_000, emu_dir, stop_after)
        replay = None
        deterministic = None
        if first["classification"] == "pass":
            replay = await capture_run(
                entry, output, "run-2", 200_000_000, emu_dir, stop_after)
            deterministic = replay["classification"] == "pass" and \
                            replay_signature(first) == replay_signature(replay)
        flash = image_dir / ("run-1-extended" if first["run"].endswith("extended")
                             else "run-1") / "flash.bin"
        after = inventory_report(flash) if flash.exists() else {"valid": False, "blocks": []}
        write_json(image_dir / "eeprom-after.json", after)
        write_json(image_dir / "eeprom-diff.json", inventory_diff(before, after))
        row = {"slug": entry["slug"], "classification": first["classification"],
               "reason": first["reason"], "run": first["run"],
               "deterministic_replay": deterministic,
               "captured_frames": len(first["captures"]),
               "terminal": first["terminal"]}
        matrix.append(row)
        write_json(image_dir / "result.json", row)
    write_json(output / "reachability.json", matrix)
    return matrix


async def retry_progressing(manifest: dict[str, Any], output: Path,
                            emu_dir: Path,
                            stop_after: str = PAGE_NAMES[-1]) -> None:
    """Give nonterminal first runs their single allowed 400M-tick extension."""
    by_slug = {entry["slug"]: entry for entry in manifest["images"]}
    for result_path in sorted(output.glob("*/result.json")):
        row = json.loads(result_path.read_text(encoding="utf-8"))
        if row["classification"] not in ("unreachable", "timeout"):
            continue
        if row["terminal"]["serial_exit"] or row["terminal"]["icount"] <= 0:
            continue
        if not any(marker in row["reason"] for marker in
                   ("wall-clock timeout", "tick limit", "UI socket closed",
                    "information code produced no screen change")):
            continue
        entry = by_slug[row["slug"]]
        image_dir = result_path.parent
        first = await capture_run(
            entry, output, "run-1-extended", 400_000_000, emu_dir, stop_after)
        replay = None
        deterministic = None
        if first["classification"] == "pass":
            replay = await capture_run(
                entry, output, "run-2", 200_000_000, emu_dir, stop_after)
            deterministic = (replay["classification"] == "pass" and
                             replay_signature(first) == replay_signature(replay))
        source = emu_dir.parent / entry["source_path"]
        before_path = image_dir / "eeprom-before.json"
        before = (json.loads(before_path.read_text(encoding="utf-8"))
                  if before_path.exists() else inventory_report(source))
        flash = image_dir / "run-1-extended/flash.bin"
        after = inventory_report(flash) if flash.exists() else {
            "valid": False, "blocks": []}
        write_json(image_dir / "eeprom-after.json", after)
        write_json(image_dir / "eeprom-diff.json", inventory_diff(before, after))
        updated = {
            "slug": entry["slug"], "classification": first["classification"],
            "reason": first["reason"], "run": first["run"],
            "deterministic_replay": deterministic,
            "captured_frames": len(first["captures"]),
            "terminal": first["terminal"],
        }
        write_json(result_path, updated)


async def resume_matrix(manifest: dict[str, Any], output: Path,
                        emu_dir: Path, only: set[str], replay_enabled: bool,
                        stop_after: str = PAGE_NAMES[-1]) -> None:
    """Resume calibrated captures without discarding an already valid first run."""
    for entry in manifest["images"]:
        if entry["slug"] not in only:
            continue
        image_dir = output / entry["slug"]
        source = emu_dir.parent / entry["source_path"]
        before_path = image_dir / "eeprom-before.json"
        before = (json.loads(before_path.read_text(encoding="utf-8"))
                  if before_path.exists() else inventory_report(source))
        write_json(before_path, before)
        first_path = image_dir / "run-1/capture.json"
        first = (json.loads(first_path.read_text(encoding="utf-8"))
                 if first_path.exists() else
                 await capture_run(
                     entry, output, "run-1", 200_000_000, emu_dir, stop_after))
        deterministic = None
        if first["classification"] == "pass" and replay_enabled:
            replay = await capture_run(
                entry, output, "run-2-replay", 200_000_000, emu_dir, stop_after)
            deterministic = (replay["classification"] == "pass" and
                             replay_signature(first) == replay_signature(replay))
        flash = image_dir / "run-1/flash.bin"
        after = inventory_report(flash) if flash.exists() else {
            "valid": False, "blocks": []}
        write_json(image_dir / "eeprom-after.json", after)
        write_json(image_dir / "eeprom-diff.json", inventory_diff(before, after))
        row = {
            "slug": entry["slug"], "classification": first["classification"],
            "reason": first["reason"], "run": first["run"],
            "deterministic_replay": deterministic,
            "captured_frames": len(first["captures"]), "terminal": first["terminal"],
        }
        write_json(image_dir / "result.json", row)


def verify_matrix(manifest: dict[str, Any], output: Path,
                  stop_after: str = PAGE_NAMES[-1]) -> list[str]:
    failures: list[str] = []
    for entry in manifest["images"]:
        result_path = output / entry["slug"] / "result.json"
        if not result_path.exists():
            failures.append(f"{entry['slug']}: missing result")
            continue
        row = json.loads(result_path.read_text())
        if row["classification"] == "pass" and not row["deterministic_replay"]:
            failures.append(f"{entry['slug']}: replay mismatch")
        image_dir = output / entry["slug"]
        capture = image_dir / row["run"] / "capture.json"
        data = json.loads(capture.read_text())
        has_fsn = "--fsn" in data["command"]
        has_imei = "--imei" in data["command"]
        if has_fsn != (entry["identity_mode"] == "native-fsn"):
            failures.append(f"{entry['slug']}: capture/manifest identity mismatch")
        if has_imei != (entry["identity_mode"] == "native-fsn" and
                        entry["device"] == "m55"):
            failures.append(f"{entry['slug']}: capture/manifest IMEI mismatch")
        after_path = image_dir / "eeprom-after.json"
        if not after_path.exists() or not json.loads(
                after_path.read_text(encoding="utf-8")).get("valid"):
            failures.append(f"{entry['slug']}: invalid logical EEPROM report")
        for run_capture in sorted(image_dir.glob("run-*/capture.json")):
            run_data = json.loads(run_capture.read_text(encoding="utf-8"))
            if run_data["terminal"]["ui_protocol_errors"]:
                failures.append(f"{entry['slug']}: UI protocol error in {run_data['run']}")
            if run_data["classification"] != "pass":
                continue
            if run_data["terminal"]["serial_exit"]:
                failures.append(f"{entry['slug']}: pass contains serial EXIT")
            if not run_data.get("trace_validation", {}).get("matching_down_up_pairs"):
                failures.append(f"{entry['slug']}: keypad mismatch in {run_data['run']}")
            expected_pages = pages_through(stop_after)
            families = {item["page"] for item in run_data["captures"]}
            if families != set(expected_pages):
                failures.append(f"{entry['slug']}: incomplete {run_data['run']}")
            if run_data.get("stop_after", PAGE_NAMES[-1]) != stop_after:
                failures.append(f"{entry['slug']}: wrong stop point in {run_data['run']}")
            if stop_after == "status":
                if any(item["page"] == "cc-monitor" for item in run_data["captures"]):
                    failures.append(f"{entry['slug']}: status run contains CC frame")
                if sum(action["key"] == "soft-left"
                       for action in run_data["actions"]) != 1:
                    failures.append(f"{entry['slug']}: status run contains CC action")
                status_positions = [item["position"] for item in run_data["captures"]
                                    if item["page"] == "status"]
                if not status_positions or status_positions != list(
                        range(1, max(status_positions) + 1)):
                    failures.append(f"{entry['slug']}: incomplete Status scroll positions")
            if any(action["release"] != "after-sample"
                   for action in run_data["actions"]):
                failures.append(f"{entry['slug']}: non-sampled release in {run_data['run']}")
    return failures


def read_generated_png(path: Path) -> tuple[int, int, list[tuple[int, int, int, int]]]:
    raw = path.read_bytes()
    if raw[:8] != b"\x89PNG\r\n\x1a\n":
        raise CaptureError(f"not a PNG: {path}")
    offset, width, height = 8, 0, 0
    compressed = bytearray()
    while offset < len(raw):
        size = struct.unpack_from(">I", raw, offset)[0]
        kind = raw[offset + 4:offset + 8]
        payload = raw[offset + 8:offset + 8 + size]
        offset += 12 + size
        if kind == b"IHDR":
            width, height, depth, color, _c, _f, _i = struct.unpack(">IIBBBBB", payload)
            if depth != 8 or color != 6:
                raise CaptureError(f"unsupported generated PNG format: {path}")
        elif kind == b"IDAT":
            compressed.extend(payload)
        elif kind == b"IEND":
            break
    raster = zlib.decompress(compressed)
    stride = width * 4
    pixels: list[tuple[int, int, int, int]] = []
    for y in range(height):
        row = raster[y * (stride + 1):(y + 1) * (stride + 1)]
        if not row or row[0] != 0:
            raise CaptureError(f"unsupported generated PNG filter: {path}")
        pixels.extend(tuple(row[i:i + 4]) for i in range(1, len(row), 4))
    return width, height, pixels


def create_contact_sheet(output: Path, manifest: dict[str, Any], device: str) -> None:
    entries: list[tuple[dict[str, Any], dict[str, Any], Path]] = []
    by_slug = {entry["slug"]: entry for entry in manifest["images"]}
    for result_path in sorted(output.glob(f"{device}-*/result.json")):
        result = json.loads(result_path.read_text(encoding="utf-8"))
        if result["classification"] != "pass":
            continue
        run_dir = result_path.parent / result["run"]
        capture = json.loads((run_dir / "capture.json").read_text(encoding="utf-8"))
        for frame in capture["captures"]:
            entries.append((by_slug[result["slug"]], frame,
                            run_dir / "frames" / frame["native_png"]))
    if not entries:
        return
    width, height, _pixels = read_generated_png(entries[0][2])
    columns, gap = 8, 2
    rows = (len(entries) + columns - 1) // columns
    sheet_width = columns * width + (columns - 1) * gap
    sheet_height = rows * height + (rows - 1) * gap
    canvas = [(255, 255, 255, 255)] * (sheet_width * sheet_height)
    mapping = []
    for index, (entry, frame, path) in enumerate(entries):
        image_width, image_height, pixels = read_generated_png(path)
        if (image_width, image_height) != (width, height):
            raise CaptureError("mixed LCD geometry in contact sheet")
        x = (index % columns) * (width + gap)
        y = (index // columns) * (height + gap)
        for row in range(height):
            dst = (y + row) * sheet_width + x
            src = row * width
            canvas[dst:dst + width] = pixels[src:src + width]
        mapping.append({"index": index, "x": x, "y": y,
                        "slug": entry["slug"], "identity_mode": entry["identity_mode"],
                        "page": frame["page"], "position": frame["position"],
                        "raw_rgb_sha256": frame["raw_rgb_sha256"],
                        "source_png": str(path.relative_to(output))})
    write_png(output / f"contact-sheet-{device}.png", canvas,
              sheet_width, sheet_height)
    write_json(output / f"contact-sheet-{device}.json", mapping)


def refresh_screen_data(output: Path, manifest: dict[str, Any]) -> None:
    """Refresh hash bindings without overwriting the manual transcription."""
    path = output / "screen-data.json"
    existing = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}
    manual = {item["slug"]: item.get("transcription")
              for item in existing.get("images", [])}
    manual_path = output / "manual-transcriptions.json"
    if manual_path.exists():
        manual.update(json.loads(manual_path.read_text(encoding="utf-8")))
    images = []
    for entry in manifest["images"]:
        result_path = output / entry["slug"] / "result.json"
        if not result_path.exists():
            continue
        result = json.loads(result_path.read_text(encoding="utf-8"))
        frame_hashes = {page: [] for page in PAGE_NAMES}
        if result["classification"] == "pass":
            capture = json.loads((result_path.parent / result["run"] /
                                  "capture.json").read_text(encoding="utf-8"))
            for frame in capture["captures"]:
                frame_hashes[frame["page"]].append({
                    "position": frame["position"],
                    "raw_rgb_sha256": frame["raw_rgb_sha256"],
                })
        images.append({
            "slug": entry["slug"], "source_sha256": entry["source_sha256"],
            "identity_mode": entry["identity_mode"],
            "classification": result["classification"],
            "frame_hashes": frame_hashes,
            "transcription": manual.get(entry["slug"]),
        })
    data = {
        "schema": "x55-live-info-menu-screen-data", "schema_version": 1,
        "method": ("Manual reading of the retained scaled PNGs; each transcription "
                   "is bound to every ordered raw-RGB hash in the same image record."),
        "images": images,
    }
    write_json(path, data)
    lines = ["# Live x55 info-menu transcription", "", data["method"], "",
             "`built-in-eeprom` identifies the explicitly synthesized fallback requested for "
             "dumps whose native FSN is unknown.", "",
             "| Image | Identity | Result | IMEI | Version | Product date | Variant |",
             "|---|---|---|---|---|---|---|"]
    for item in images:
        tx = item["transcription"] or {}
        status = tx.get("status", {})
        lines.append(
            f"| `{item['slug']}` | {item['identity_mode']} | {item['classification']} | "
            f"{tx.get('imei', '')} | {status.get('sw_version', '')} | "
            f"{status.get('production_date', '')} | {status.get('variant', '')} |")
    for item in images:
        if not item["transcription"]:
            continue
        lines += ["", f"## {item['slug']}", "", "Normalized/manual data:", "", "```json",
                  json.dumps(item["transcription"], indent=2, sort_keys=True), "```", "",
                  "Ordered raw-RGB frame hashes:", ""]
        for page in PAGE_NAMES:
            hashes = ", ".join(
                f"{frame['position']}=`{frame['raw_rgb_sha256']}`"
                for frame in item["frame_hashes"][page])
            lines.append(f"- {page}: {hashes}")
    (output / "screen-data.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def refresh_evidence(manifest: dict[str, Any], output: Path) -> None:
    by_slug = {entry["slug"]: entry for entry in manifest["images"]}
    rows = []
    for result_path in sorted(output.glob("*/result.json")):
        result = json.loads(result_path.read_text(encoding="utf-8"))
        entry = by_slug[result["slug"]]
        rows.append({**result, "device": entry["device"],
                     "identity_mode": entry["identity_mode"]})
        for capture_path in sorted(result_path.parent.glob("run-*/capture.json")):
            capture = json.loads(capture_path.read_text(encoding="utf-8"))
            trace = capture_path.parent / "trace/trace.parquet"
            if trace.exists():
                report = create_eeprom_trace_report([trace])
                write_json(capture_path.parent / "eeprom-trace.json", report)
                (capture_path.parent / "eeprom-trace.txt").write_text(
                    format_eeprom_trace_report(report), encoding="utf-8")
                capture["trace_validation"] = validate_action_trace(
                    trace, capture["actions"])
                write_json(capture_path, capture)
    write_json(output / "reachability.json", rows)
    lines = ["# Live x55 info-menu reachability", "",
             "Live captures use exact source bytes and no SIM. `native-fsn` preserves the dump's "
             "logical EEPROM; `built-in-eeprom` is an explicitly synthesized identity fallback.", "",
             "| Image | Device | Identity | Result | Frames | Replay | Icount |",
             "|---|---|---|---|---:|---|---:|"]
    for row in rows:
        lines.append(f"| `{row['slug']}` | {row['device'].upper()} | "
                     f"{row['identity_mode']} | {row['classification']} | "
                     f"{row['captured_frames']} | {row['deterministic_replay']} | "
                     f"{row['terminal']['icount']} |")
    (output / "reachability.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    command_lines = ["# Commands are reproduced from source-manifest.json; <socket> is per-run."]
    for entry in manifest["images"]:
        command_lines.append(" ".join(entry["command_template"]))
    (output / "commands.txt").write_text("\n".join(command_lines) + "\n", encoding="utf-8")
    for device in ("c55", "m55"):
        create_contact_sheet(output, manifest, device)
    refresh_screen_data(output, manifest)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path,
                        default=Path(__file__).resolve().parents[2])
    sub = parser.add_subparsers(dest="command", required=True)
    p_manifest = sub.add_parser("manifest")
    p_manifest.add_argument("--output", type=Path, required=True)
    p_manifest.add_argument("--cache", type=Path)
    p_capture = sub.add_parser("capture")
    p_capture.add_argument("--manifest", type=Path, required=True)
    p_capture.add_argument("--output", type=Path, required=True)
    p_capture.add_argument("--only", action="append", default=[])
    p_capture.add_argument("--stop-after", choices=PAGE_NAMES,
                           default=PAGE_NAMES[-1])
    p_verify = sub.add_parser("verify")
    p_verify.add_argument("--manifest", type=Path, required=True)
    p_verify.add_argument("--output", type=Path, required=True)
    p_verify.add_argument("--stop-after", choices=PAGE_NAMES,
                          default=PAGE_NAMES[-1])
    p_refresh = sub.add_parser("refresh")
    p_refresh.add_argument("--manifest", type=Path, required=True)
    p_refresh.add_argument("--output", type=Path, required=True)
    p_retry = sub.add_parser("retry")
    p_retry.add_argument("--manifest", type=Path, required=True)
    p_retry.add_argument("--output", type=Path, required=True)
    p_retry.add_argument("--stop-after", choices=PAGE_NAMES,
                         default=PAGE_NAMES[-1])
    p_resume = sub.add_parser("resume")
    p_resume.add_argument("--manifest", type=Path, required=True)
    p_resume.add_argument("--output", type=Path, required=True)
    p_resume.add_argument("--only", action="append", required=True)
    p_resume.add_argument("--no-replay", action="store_true")
    p_resume.add_argument("--stop-after", choices=PAGE_NAMES,
                          default=PAGE_NAMES[-1])
    args = parser.parse_args(argv)
    repo = args.repo.resolve()
    if args.command == "manifest":
        write_json(args.output, build_manifest(repo, args.cache))
        return 0
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    if args.command == "capture":
        asyncio.run(run_matrix(manifest, args.output.resolve(), repo / "emu",
                               set(args.only), args.stop_after))
        return 0
    if args.command == "retry":
        asyncio.run(retry_progressing(
            manifest, args.output.resolve(), repo / "emu", args.stop_after))
        refresh_evidence(manifest, args.output.resolve())
        return 0
    if args.command == "resume":
        asyncio.run(resume_matrix(
            manifest, args.output.resolve(), repo / "emu", set(args.only),
            not args.no_replay, args.stop_after))
        refresh_evidence(manifest, args.output.resolve())
        return 0
    if args.command == "refresh":
        refresh_evidence(manifest, args.output.resolve())
        return 0
    failures = verify_matrix(manifest, args.output, args.stop_after)
    for failure in failures:
        print(failure)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
