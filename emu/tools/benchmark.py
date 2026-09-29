#!/usr/bin/env python3
"""Repeatable end-to-end throughput benchmarks for CEMU and QEMU."""

import argparse
import contextlib
import datetime
import hashlib
import json
import os
import platform
import resource
import socket
import statistics
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools import ui_protocol
from tools.bundled_firmware import entry as bundled_entry

UI_MAGIC = 0x55353543
UI_VERSIONS = {"cemu": ui_protocol.VERSION, "qemu": ui_protocol.VERSION}
SUPPORTED_UI_VERSIONS = (7, ui_protocol.VERSION)
UI_VERSION = UI_VERSIONS["cemu"]
UI_HEADER = ui_protocol.HEADER
QEMU_HEADER = struct.Struct("<IHHIQQI")
UI_HELLO = 1
UI_FRAME = 2
UI_STATS = 3
UI_KEY = 4
UI_RELEASE_ALL = 5
UI_KEY_RELEASE_AFTER_SAMPLE = 6
UI_STATS_PAYLOAD = ui_protocol.STATS_PAYLOAD
QEMU_STATS_PAYLOAD = struct.Struct("<QQQI")
UI_MAX_PAYLOAD = 128 * 162 * 3


SCHEMA = 2
CROSS_ENGINE_SCHEMA = 3
QEMU_THROUGHPUT_SCHEMA = 4
CEMU_FSN = bundled_entry("c55")["identity"]["value"]
EMU_RUNNER = Path(__file__).resolve().parents[1] / "bin" / "emu"
CANONICAL_C55_SHA256 = bundled_entry("c55")["sha256"]
UI_BOOT_DIGEST = None  # No bundled-image UI milestone has been qualified.
UI_SOFTKEY_DIGEST = None
RATE_KEYS = (
    "emulator_ticks_per_s",
    "emulator_guest_instructions_per_s",
    "process_wall_ticks_per_s",
    "child_cpu_ticks_per_s",
)
QEMU_RATE_KEYS = (
    "process_wall_ticks_per_s",
    "child_cpu_ticks_per_s",
    "process_wall_guest_instructions_per_s",
    "child_cpu_guest_instructions_per_s",
)
QEMU_THROUGHPUT_WARMUP_TICKS = 4_000_000
QEMU_THROUGHPUT_WINDOW_S = 2.0
QEMU_QUALIFICATION_BASELINES = None  # Bundled-image performance is unqualified.
SCENARIOS = {
    "active-reset": {
        "description": "One million reset-boot ticks without loop observation",
        "arguments": ["--limit", "1m"],
    },
    "no-sim-boot": {
        "description": "Twenty million ticks of the normal no-SIM boot",
        "arguments": ["--limit", "20m"],
    },
    "sim-stub-boot": {
        "description": "Twenty million ticks with the SIM and synthetic GSM responder",
        "arguments": ["--limit", "20m", "--sim", "--synth", "gsm"],
    },
    "post-boot": {
        "description": "Fifty million additional ticks from a 20M no-SIM snapshot",
        "arguments": ["--limit", "50m"],
    },
    "ui-connected": {
        "description": "Configurable tick bound while a Unix-socket consumer drains frames",
        "arguments": ["--limit", "5m"],
    },
    "active-reset-monitor": {
        "description": "One million reset-boot ticks with monitor collection enabled",
        "arguments": ["--limit", "1m", "--monitor"],
        "instrumented": True,
    },
    "ui-boot": {
        "description": "Launch to the canonical C55 Insert SIM frame",
        "ui_milestone": True,
        "target_digest": UI_BOOT_DIGEST,
    },
    "ui-softkey": {
        "description": "Canonical Insert SIM soft-left to the confirmation dialog",
        "ui_milestone": True,
        "target_digest": UI_SOFTKEY_DIGEST,
        "input": "soft-left",
    },
    "qemu-no-sim-throughput": {
        "description": "Supervised C55 no-SIM throughput after four million ticks",
        "qemu_throughput": True,
        "sim": False,
    },
    "qemu-sim-throughput": {
        "description": "Supervised C55 SIM throughput after four million ticks",
        "qemu_throughput": True,
        "sim": True,
    },
}

LEGACY_SCENARIOS = tuple(
    name for name, config in SCENARIOS.items() if not config.get("ui_milestone")
)
UI_SCENARIOS = tuple(
    name for name, config in SCENARIOS.items() if config.get("ui_milestone")
)
QEMU_THROUGHPUT_SCENARIOS = tuple(
    name for name, config in SCENARIOS.items()
    if config.get("qemu_throughput")
)
MILESTONE_MEDIAN_KEYS = (
    "emulator_elapsed_s",
    "preparation_wall_s",
    "process_wall_s",
    "child_cpu_s",
    "process_wall_ticks_per_s",
    "child_cpu_ticks_per_s",
    "process_wall_guest_instructions_per_s",
    "child_cpu_guest_instructions_per_s",
)


class BenchmarkError(RuntimeError):
    pass


def parse_cemu_json(stdout):
    text = stdout.strip()
    if not text:
        raise BenchmarkError("cemu produced no benchmark JSON")
    try:
        value = json.loads(text)
    except ValueError as exc:
        raise BenchmarkError("invalid cemu benchmark JSON: %s" % exc)
    if not isinstance(value, dict) or value.get("schema") != SCHEMA:
        raise BenchmarkError("unsupported cemu benchmark schema")
    required = {
        "status", "device", "start_icount", "end_icount", "ticks",
        "guest_instructions", "elapsed_s", "ticks_per_s",
        "guest_instructions_per_s", "pc", "state_digest",
    }
    missing = sorted(required.difference(value))
    if missing:
        raise BenchmarkError("cemu benchmark JSON is missing: %s" % ", ".join(missing))
    return value


def _child_cpu_seconds(usage):
    return usage.ru_utime + usage.ru_stime


def run_cemu(command, cwd=None):
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    started = time.perf_counter()
    completed = subprocess.run(
        command,
        cwd=str(cwd) if cwd is not None else None,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        universal_newlines=True,
    )
    wall_s = time.perf_counter() - started
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    child_cpu_s = _child_cpu_seconds(after) - _child_cpu_seconds(before)
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip() or "no output"
        raise BenchmarkError(
            "cemu exited %d: %s\ncommand: %s"
            % (completed.returncode, detail, " ".join(command))
        )

    stats = parse_cemu_json(completed.stdout)
    if stats["status"] != "limit":
        raise BenchmarkError(
            "scenario stopped with status %s after %s ticks"
            % (stats["status"], stats["ticks"])
        )
    ticks = stats["ticks"]
    return {
        "command": command,
        "status": stats["status"],
        "start_icount": stats["start_icount"],
        "end_icount": stats["end_icount"],
        "ticks": ticks,
        "guest_instructions": stats["guest_instructions"],
        "pc": stats["pc"],
        "state_digest": stats["state_digest"],
        "emulator_elapsed_s": stats["elapsed_s"],
        "process_wall_s": wall_s,
        "child_cpu_s": child_cpu_s,
        "emulator_ticks_per_s": stats["ticks_per_s"],
        "emulator_guest_instructions_per_s": stats["guest_instructions_per_s"],
        "process_wall_ticks_per_s": ticks / wall_s if wall_s > 0 else None,
        "child_cpu_ticks_per_s": ticks / child_cpu_s if child_cpu_s > 0 else None,
        "interrupt_cache_queries": stats.get("interrupt_cache_queries", 0),
        "interrupt_cache_hits": stats.get("interrupt_cache_hits", 0),
        "interrupt_cache_scans": stats.get("interrupt_cache_scans", 0),
        "interrupt_cache_invalidations": stats.get(
            "interrupt_cache_invalidations", 0
        ),
    }


def parse_count(value):
    suffix = value[-1:].lower()
    if suffix == "m":
        return int(value[:-1]) * 1000000
    if suffix == "k":
        return int(value[:-1]) * 1000
    return int(value, 0)


def encode_ui_packet(message_type, payload=b"", sequence=0, icount=0, *, version=UI_VERSION, run_id=bytes(16)):
    if version not in SUPPORTED_UI_VERSIONS:
        raise BenchmarkError("unsupported UI protocol version")
    if version == UI_VERSION:
        return ui_protocol.encode_packet(message_type, payload, sequence=sequence,
                                         icount=icount, run_id=run_id)
    return QEMU_HEADER.pack(
        UI_MAGIC, version, message_type, len(payload), sequence, icount, 0
    ) + payload


def decode_ui_packets(buffer, *, version=UI_VERSION, identity=None):
    if version not in SUPPORTED_UI_VERSIONS:
        raise BenchmarkError("unsupported UI protocol version")
    if version == UI_VERSION:
        try:
            packets, rest = ui_protocol.decode_packets(buffer)
        except ui_protocol.ProtocolError as exc:
            raise BenchmarkError(str(exc)) from exc
        if identity is not None:
            try:
                for packet in packets:
                    ui_protocol.observe_packet(packet, identity)
            except ui_protocol.ProtocolError as exc:
                raise BenchmarkError(str(exc)) from exc
        return [(p.message_type, p.payload, p.sequence, p.icount) for p in packets], rest
    packets = []
    offset = 0
    while len(buffer) - offset >= QEMU_HEADER.size:
        fields = QEMU_HEADER.unpack(buffer[offset:offset + QEMU_HEADER.size])
        magic, received_version, message_type, length, sequence, icount, reserved = fields
        if magic != UI_MAGIC or received_version != version or reserved != 0:
            raise BenchmarkError("invalid UI protocol header")
        if length > UI_MAX_PAYLOAD:
            raise BenchmarkError("oversized UI protocol payload")
        end = offset + QEMU_HEADER.size + length
        if end > len(buffer):
            break
        payload = buffer[offset + QEMU_HEADER.size:end]
        packets.append((message_type, payload, sequence, icount))
        offset = end
    return packets, buffer[offset:]


def decode_ui_hello(payload, *, version=UI_VERSION):
    if version != 7:
        try:
            width, height, model, keys, asc0, audio = ui_protocol.decode_hello(payload)
        except ui_protocol.ProtocolError as exc:
            raise BenchmarkError(str(exc)) from exc
        return {"width": width, "height": height, "model": model,
                "keys": list(keys),
                "capabilities": (ui_protocol.CAP_ASC0 if asc0 else 0) |
                                (ui_protocol.CAP_AUDIO if audio else 0)}
    # QEMU retains its explicitly selected v7 descriptor contract.
    if len(payload) < 9:
        raise BenchmarkError("short UI HELLO payload")
    width, height, key_count, capabilities, model_length = struct.unpack_from(
        "<HHHHB", payload
    )
    offset = 9
    if offset + model_length > len(payload):
        raise BenchmarkError("truncated UI model name")
    model = payload[offset:offset + model_length].decode("ascii")
    offset += model_length
    keys = []
    for unused_index in range(key_count):
        del unused_index
        if offset >= len(payload):
            raise BenchmarkError("truncated UI key catalog")
        length = payload[offset]
        offset += 1
        if offset + length > len(payload):
            raise BenchmarkError("truncated UI key name")
        keys.append(payload[offset:offset + length].decode("ascii"))
        offset += length
    if offset != len(payload):
        raise BenchmarkError("UI HELLO payload has trailing bytes")
    return {
        "width": width,
        "height": height,
        "capabilities": capabilities,
        "model": model,
        "keys": keys,
    }


def _process_cpu_seconds(pid):
    """Return one live child's CPU time without waiting for it to exit."""
    try:
        stat_text = Path("/proc") / str(pid) / "stat"
        fields = stat_text.read_text(encoding="ascii").rsplit(")", 1)[1].split()
        # fields begins with process field 3; utime/stime are fields 14/15.
        ticks = int(fields[11]) + int(fields[12])
        return ticks / os.sysconf("SC_CLK_TCK")
    except (AttributeError, IndexError, OSError, ValueError):
        return None


def _process_tree_cpu_seconds(pid, seen=None):
    """Return CPU time for a live supervisor and all of its descendants."""
    seen = set() if seen is None else seen
    if pid in seen:
        return 0.0
    seen.add(pid)
    own = _process_cpu_seconds(pid)
    total = own or 0.0
    found = own is not None
    try:
        children = (Path("/proc") / str(pid) / "task" / str(pid) /
                    "children").read_text(encoding="ascii").split()
    except OSError:
        children = ()
    for child in children:
        value = _process_tree_cpu_seconds(int(child), seen)
        if value is not None:
            total += value
            found = True
    return total if found else None


def _terminate_process(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def _unlink_if_exists(path):
    try:
        path.unlink()
    except FileNotFoundError:
        pass


def _ui_process_error(engine, process, stderr_path):
    detail = stderr_path.read_text(encoding="utf-8", errors="replace").strip()
    if not detail:
        detail = "no diagnostic output"
    return BenchmarkError(
        "%s UI process exited %s before the milestone: %s"
        % (engine, process.returncode, detail)
    )


def _milestone_command(engine, executable, flash, socket_path, extra_args,
                       qemu_supervisor=None):
    if engine == "qemu":
        return [
            str(qemu_supervisor or EMU_RUNNER), "run", flash,
            "--engine", "qemu",
            "--qemu-binary", executable,
            "--fsn", CEMU_FSN,
            "--ui-socket", str(socket_path),
        ] + list(extra_args)
    if engine == "cemu":
        return [
            executable, flash,
            "--device", "c55",
            "--fsn", CEMU_FSN,
            "--limit", "100m",
            "--ui-socket", str(socket_path),
        ] + list(extra_args)
    raise BenchmarkError("unsupported UI benchmark engine: %s" % engine)


def _ui_delta(end, start, key):
    if end is None:
        return None
    before = 0 if start is None else start[key]
    return end[key] - before


def run_ui_milestone(
        engine, executable, flash, name, runtime_dir, extra_args=(),
        timeout_s=180.0, qemu_supervisor=None):
    config = SCENARIOS[name]
    version = UI_VERSIONS[engine]
    identity = {}
    stats_codec = UI_STATS_PAYLOAD
    socket_path = Path(runtime_dir) / ("benchmark-%s-ui.sock" % engine)
    stderr_path = Path(runtime_dir) / ("benchmark-%s-ui.stderr" % engine)
    _unlink_if_exists(socket_path)
    command = _milestone_command(
        engine, executable, flash, socket_path, extra_args,
        qemu_supervisor=qemu_supervisor,
    )
    launched = time.perf_counter()
    with stderr_path.open("wb") as stderr:
        process = subprocess.Popen(
            command, cwd=str(runtime_dir), stdout=subprocess.DEVNULL,
            stderr=stderr,
        )
    client = None
    try:
        client = _connect_ui_socket(process, socket_path, engine=engine)
        deadline = time.monotonic() + timeout_s
        buffer = b""
        hello = None
        latest_stats = None
        packets = {"hello": 0, "stats": 0, "frame": 0}
        frame_digests = []
        start_wall = launched
        start_cpu = 0.0
        start_stats = None
        preparation_wall_s = None
        input_sent = name == "ui-boot"

        while time.monotonic() < deadline:
            if process.poll() is not None:
                error = _ui_process_error(engine, process, stderr_path)
                raise BenchmarkError(
                    "%s; observed frame digests: %s" %
                    (error, ",".join(frame_digests[-12:]) or "none")
                )
            try:
                chunk = client.recv(65536)
            except socket.timeout:
                continue
            except ConnectionResetError:
                try:
                    process.wait(timeout=1.0)
                except subprocess.TimeoutExpired:
                    pass
                raise _ui_process_error(engine, process, stderr_path)
            if not chunk:
                if process.poll() is None:
                    raise BenchmarkError(
                        "%s UI socket closed before milestone; observed frame digests: %s" %
                        (engine, ",".join(frame_digests[-12:]) or "none")
                    )
                error = _ui_process_error(engine, process, stderr_path)
                raise BenchmarkError(
                    "%s; observed frame digests: %s" %
                    (error, ",".join(frame_digests[-12:]) or "none")
                )
            buffer += chunk
            decoded, buffer = decode_ui_packets(buffer, version=version, identity=identity)
            for message_type, payload, unused_sequence, unused_icount in decoded:
                del unused_sequence, unused_icount
                if message_type == UI_HELLO:
                    packets["hello"] += 1
                    hello = decode_ui_hello(payload, version=version)
                    if hello["model"] != "c55" or (hello["width"], hello["height"]) != (101, 64):
                        raise BenchmarkError(
                            "%s reported unexpected UI geometry/model: %r" %
                            (engine, hello)
                        )
                elif message_type == UI_STATS:
                    packets["stats"] += 1
                    if len(payload) != stats_codec.size:
                        raise BenchmarkError("invalid UI STATS payload")
                    elapsed_ns, ticks, guest_instructions, pc = ui_protocol.decode_stats(payload)[:4]
                    latest_stats = {
                        "elapsed_ns": elapsed_ns,
                        "ticks": ticks,
                        "guest_instructions": guest_instructions,
                        "pc": pc,
                    }
                    if preparation_wall_s is None:
                        preparation_wall_s = time.perf_counter() - launched
                elif message_type == UI_FRAME:
                    packets["frame"] += 1
                    digest = hashlib.sha256(payload).hexdigest()
                    frame_digests.append(digest)
                    if name == "ui-softkey" and not input_sent and digest == UI_BOOT_DIGEST:
                        if hello is None or latest_stats is None:
                            raise BenchmarkError(
                                "%s reached Insert SIM before HELLO/STATS" % engine
                            )
                        try:
                            key_index = hello["keys"].index(config["input"])
                        except ValueError:
                            raise BenchmarkError(
                                "%s UI omits key %s" % (engine, config["input"])
                            )
                        start_stats = dict(latest_stats)
                        start_cpu = _process_tree_cpu_seconds(process.pid)
                        packets = {"hello": 0, "stats": 0, "frame": 0}
                        frame_digests = []
                        start_wall = time.perf_counter()
                        client.sendall(encode_ui_packet(
                            UI_KEY, bytes((key_index, 1)), version=version,
                            run_id=identity.get("run_id", bytes(16))
                        ))
                        client.sendall(encode_ui_packet(
                            UI_KEY_RELEASE_AFTER_SAMPLE, bytes((key_index,)), version=version,
                            run_id=identity.get("run_id", bytes(16))
                        ))
                        input_sent = True
                        continue
                    if input_sent and digest == config["target_digest"]:
                        wall_s = time.perf_counter() - start_wall
                        end_cpu = _process_tree_cpu_seconds(process.pid)
                        cpu_s = None
                        if end_cpu is not None and start_cpu is not None:
                            cpu_s = end_cpu - start_cpu
                        ticks = _ui_delta(latest_stats, start_stats, "ticks")
                        guest = _ui_delta(
                            latest_stats, start_stats, "guest_instructions"
                        )
                        elapsed_ns = _ui_delta(
                            latest_stats, start_stats, "elapsed_ns"
                        )
                        return {
                            "status": "milestone",
                            "milestone": name,
                            "target_frame_digest": config["target_digest"],
                            "final_frame_digest": digest,
                            "frame_digests": frame_digests,
                            "process_wall_s": wall_s,
                            "emulator_elapsed_s":
                                elapsed_ns / 1_000_000_000
                                if elapsed_ns is not None else None,
                            "preparation_wall_s": preparation_wall_s,
                            "child_cpu_s": cpu_s,
                            "ticks": ticks,
                            "guest_instructions": guest,
                            "pc": latest_stats["pc"] if latest_stats else None,
                            "process_wall_ticks_per_s":
                                ticks / wall_s if ticks is not None and wall_s > 0 else None,
                            "child_cpu_ticks_per_s":
                                ticks / cpu_s if ticks is not None and cpu_s else None,
                            "process_wall_guest_instructions_per_s":
                                guest / wall_s if guest is not None and wall_s > 0 else None,
                            "child_cpu_guest_instructions_per_s":
                                guest / cpu_s if guest is not None and cpu_s else None,
                            "ui_messages": packets,
                        }
        raise BenchmarkError(
            "%s %s did not reach frame %s in %.1fs; observed frame digests: %s" %
            (engine, name, config["target_digest"], timeout_s,
             ",".join(frame_digests[-12:]) or "none")
        )
    finally:
        if client is not None:
            client.close()
        _terminate_process(process)
        _unlink_if_exists(socket_path)


def run_qemu_throughput(
        executable, flash, name, runtime_dir,
        duration_s=QEMU_THROUGHPUT_WINDOW_S,
        warmup_ticks=QEMU_THROUGHPUT_WARMUP_TICKS,
        qemu_supervisor=EMU_RUNNER):
    if name not in QEMU_THROUGHPUT_SCENARIOS:
        raise BenchmarkError("unsupported QEMU throughput scenario: %s" % name)
    socket_path = Path(runtime_dir) / "benchmark-qemu-throughput-ui.sock"
    stderr_path = Path(runtime_dir) / "benchmark-qemu-throughput.stderr"
    _unlink_if_exists(socket_path)
    command = [
        str(qemu_supervisor), "run", flash,
        "--engine", "qemu", "--qemu-binary", executable,
        "--fsn", CEMU_FSN, "--ui-socket", str(socket_path),
    ]
    if SCENARIOS[name]["sim"]:
        command.append("--sim")
    with stderr_path.open("wb") as stderr:
        process = subprocess.Popen(
            command, cwd=str(runtime_dir), stdout=subprocess.DEVNULL,
            stderr=stderr,
        )
    client = None
    try:
        client = _connect_ui_socket(
            process, socket_path, timeout_s=30.0, engine="qemu"
        )
        deadline = time.monotonic() + 180.0
        buffer = b""
        identity = {}
        start_stats = None
        start_wall = None
        start_cpu = None
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise _ui_process_error("qemu", process, stderr_path)
            try:
                chunk = client.recv(65536)
            except socket.timeout:
                continue
            if not chunk:
                raise _ui_process_error("qemu", process, stderr_path)
            buffer += chunk
            decoded, buffer = decode_ui_packets(buffer, identity=identity)
            for message_type, payload, unused_sequence, unused_icount in decoded:
                del unused_sequence, unused_icount
                if message_type != UI_STATS:
                    continue
                if len(payload) != UI_STATS_PAYLOAD.size:
                    raise BenchmarkError("invalid UI STATS payload")
                elapsed_ns, ticks, guest, pc = ui_protocol.decode_stats(payload)[:4]
                stats = {
                    "elapsed_ns": elapsed_ns,
                    "ticks": ticks,
                    "guest_instructions": guest,
                    "pc": pc,
                }
                if start_stats is None:
                    if ticks < warmup_ticks:
                        continue
                    start_stats = stats
                    start_wall = time.perf_counter()
                    start_cpu = _process_tree_cpu_seconds(process.pid)
                    continue
                wall_s = time.perf_counter() - start_wall
                if wall_s < duration_s:
                    continue
                end_cpu = _process_tree_cpu_seconds(process.pid)
                cpu_s = (
                    end_cpu - start_cpu
                    if end_cpu is not None and start_cpu is not None else None
                )
                measured_ticks = stats["ticks"] - start_stats["ticks"]
                measured_guest = (
                    stats["guest_instructions"] -
                    start_stats["guest_instructions"]
                )
                emulator_s = (
                    stats["elapsed_ns"] - start_stats["elapsed_ns"]
                ) / 1_000_000_000
                return {
                    "status": "window",
                    "start_ticks": start_stats["ticks"],
                    "end_ticks": stats["ticks"],
                    "ticks": measured_ticks,
                    "guest_instructions": measured_guest,
                    "pc": stats["pc"],
                    "emulator_elapsed_s": emulator_s,
                    "process_wall_s": wall_s,
                    "child_cpu_s": cpu_s,
                    "process_wall_ticks_per_s": measured_ticks / wall_s,
                    "child_cpu_ticks_per_s": (
                        measured_ticks / cpu_s if cpu_s else None
                    ),
                    "process_wall_guest_instructions_per_s":
                        measured_guest / wall_s,
                    "child_cpu_guest_instructions_per_s": (
                        measured_guest / cpu_s if cpu_s else None
                    ),
                }
        raise BenchmarkError(
            "QEMU throughput did not complete a %.1fs window after %d ticks" %
            (duration_s, warmup_ticks)
        )
    finally:
        if client is not None:
            client.close()
        _terminate_process(process)
        _unlink_if_exists(socket_path)


def _wait_for_ui_socket(process, path, timeout_s=5.0):
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if path.exists():
            return
        if process.poll() is not None:
            break
        time.sleep(0.005)
    raise BenchmarkError("cemu did not open its UI socket")


def _connect_ui_socket(process, path, timeout_s=5.0, engine="cemu"):
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if process.poll() is not None:
            break
        client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        client.settimeout(0.1)
        try:
            client.connect(str(path))
            return client
        except (FileNotFoundError, ConnectionRefusedError):
            client.close()
            time.sleep(0.005)
        except BaseException:
            client.close()
            raise
    raise BenchmarkError("%s did not accept its UI socket connection" % engine)


def interval_distribution(values):
    """Summarize consecutive intervals, in the input timestamp's units."""
    intervals = sorted(b - a for a, b in zip(values, values[1:]))
    if not intervals:
        return {"count": 0, "min": None, "median": None, "p95": None, "max": None}
    return {"count": len(intervals), "min": intervals[0],
            "median": statistics.median(intervals),
            "p95": intervals[max(0, (95 * len(intervals) + 99) // 100 - 1)],
            "max": intervals[-1]}


def record_ui_stats(samples, payload, sequence, icount, arrival_ns):
    if len(payload) != UI_STATS_PAYLOAD.size:
        raise BenchmarkError("invalid UI STATS payload")
    (elapsed_ns, ticks, guest, pc, sample_sequence, measured_ns,
     window_ns, tick_rate, guest_rate, valid) = ui_protocol.decode_stats(payload)
    samples.append({"window_ns": window_ns, "rates_valid": bool(valid),
                    "ticks_per_s": tick_rate if valid else None,
                    "guest_instructions_per_s": guest_rate if valid else None,
                    "sample_sequence": sample_sequence, "measured_ns": measured_ns,
                    "elapsed_ns": elapsed_ns, "arrival_ns": arrival_ns,
                    "sequence": sequence, "icount": icount, "ticks": ticks,
                    "guest_instructions": guest, "pc": pc})
    return elapsed_ns, ticks, guest, pc


def run_connected_ui(cemu, flash, arguments, runtime_dir, cemu_args=()):
    limit_index = arguments.index("--limit")
    limit = parse_count(arguments[limit_index + 1])
    socket_path = Path(runtime_dir) / "benchmark-ui.sock"
    _unlink_if_exists(socket_path)
    command = (
        [cemu, flash] + list(arguments) + list(cemu_args) +
        ["--ui-socket", str(socket_path), "--benchmark-json"]
    )
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    process = subprocess.Popen(
        command, cwd=str(runtime_dir), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        universal_newlines=True,
    )
    client = None
    try:
        client = _connect_ui_socket(process, socket_path)

        buffer = b""
        stats_samples = []
        identity = {}
        lifecycle = None
        attached_icount = None
        attached_ticks = None
        attached_guest_instructions = None
        started = None
        hello_messages = 0
        stats_messages = 0
        frame_messages = 0
        final_frame_digest = None
        deadline = time.monotonic() + 5.0
        while ((attached_ticks is None or hello_messages < 1 or frame_messages < 1) and
               time.monotonic() < deadline):
            try:
                chunk = client.recv(65536)
            except socket.timeout:
                if process.poll() is not None:
                    break
                continue
            if not chunk:
                break
            arrival_ns = time.perf_counter_ns()
            buffer += chunk
            packets, buffer = decode_ui_packets(buffer, identity=identity)
            for message_type, payload, sequence, icount in packets:
                if message_type == UI_HELLO:
                    hello_messages += 1
                elif message_type == ui_protocol.LIFECYCLE:
                    lifecycle = ui_protocol.decode_lifecycle(payload)
                elif message_type == UI_STATS:
                    if len(payload) != UI_STATS_PAYLOAD.size:
                        raise BenchmarkError("invalid UI STATS payload")
                    unused_elapsed, ticks, guest_instructions, unused_pc = record_ui_stats(
                        stats_samples, payload, sequence, icount, arrival_ns)
                    del unused_elapsed, unused_pc
                    stats_messages += 1
                    if attached_ticks is None:
                        attached_icount = icount
                        attached_ticks = ticks
                        attached_guest_instructions = guest_instructions
                        started = time.perf_counter()
                elif message_type == UI_FRAME:
                    frame_messages += 1
                    final_frame_digest = hashlib.sha256(payload).hexdigest()
        if attached_ticks is None or started is None:
            raise BenchmarkError("UI frontend did not receive runtime stats")
        if hello_messages < 1 or frame_messages < 1:
            raise BenchmarkError("UI frontend did not receive handshake and frame")
        if attached_ticks >= limit:
            raise BenchmarkError("cemu reached the UI benchmark limit before attachment")

        remaining = limit - attached_ticks
        deadline = time.monotonic() + 120.0
        while True:
            if time.monotonic() >= deadline:
                raise BenchmarkError("connected UI benchmark timed out")
            try:
                chunk = client.recv(65536)
            except socket.timeout:
                continue
            if not chunk:
                break
            arrival_ns = time.perf_counter_ns()
            buffer += chunk
            packets, buffer = decode_ui_packets(buffer, identity=identity)
            for message_type, payload, sequence, icount in packets:
                if message_type == UI_HELLO:
                    hello_messages += 1
                elif message_type == ui_protocol.LIFECYCLE:
                    lifecycle = ui_protocol.decode_lifecycle(payload)
                elif message_type == UI_STATS:
                    if len(payload) != UI_STATS_PAYLOAD.size:
                        raise BenchmarkError("invalid UI STATS payload")
                    record_ui_stats(stats_samples, payload, sequence, icount, arrival_ns)
                    stats_messages += 1
                elif message_type == UI_FRAME:
                    frame_messages += 1
                    final_frame_digest = hashlib.sha256(payload).hexdigest()
        stdout, stderr = process.communicate(timeout=5.0)
        wall_s = time.perf_counter() - started
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        child_cpu_s = _child_cpu_seconds(after) - _child_cpu_seconds(before)
        if process.returncode != 0:
            detail = stderr.strip() or stdout.strip() or "no output"
            raise BenchmarkError("cemu UI run exited %d: %s" % (process.returncode, detail))
        stats = parse_cemu_json(stdout)
        if stats["status"] != "limit":
            raise BenchmarkError("cemu UI run stopped with status %s" % stats["status"])
        guest_instructions = stats["guest_instructions"] - attached_guest_instructions
        return {
            "command": command, "final_stats": stats,
            "ui_protocol_version": UI_VERSION,
            "run_id": identity.get("run_id", bytes(16)).hex(),
            "lifecycle": lifecycle,
            "final_frame_digest": final_frame_digest,
            "measurement": "socket-consumer; wall from first STATS to process completion; CPU whole process",
            "stats_samples": stats_samples,
            "stats_intervals_ns": {
                "runtime_measured": interval_distribution([s["measured_ns"] for s in stats_samples]),
                "runtime_elapsed": interval_distribution([s["elapsed_ns"] for s in stats_samples]),
                "receiver_arrival": interval_distribution([s["arrival_ns"] for s in stats_samples]),
            },
            "status": "limit", "start_icount": attached_icount,
            "end_icount": stats["end_icount"], "ticks": remaining,
            "guest_instructions": guest_instructions,
            "pc": stats["pc"], "state_digest": stats.get("state_digest"),
            "emulator_elapsed_s": None, "process_wall_s": wall_s,
            "child_cpu_s": child_cpu_s, "emulator_ticks_per_s": None,
            "emulator_guest_instructions_per_s": None,
            "process_wall_ticks_per_s": remaining / wall_s if wall_s > 0 else None,
            "child_cpu_ticks_per_s": remaining / child_cpu_s if child_cpu_s > 0 else None,
            "ui_messages": {"hello": hello_messages, "stats": stats_messages,
                            "frame": frame_messages},
        }
    except Exception:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        process.communicate()
        raise
    finally:
        if client is not None:
            client.close()


def aggregate_samples(samples):
    medians = {}
    for key in RATE_KEYS:
        values = [sample[key] for sample in samples if sample.get(key) is not None]
        medians[key] = statistics.median(values) if values else None
    return medians


def aggregate_milestone_samples(samples):
    medians = {}
    for key in MILESTONE_MEDIAN_KEYS:
        values = [sample[key] for sample in samples if sample.get(key) is not None]
        medians[key] = statistics.median(values) if values else None
    return medians


def aggregate_qemu_samples(samples):
    medians = {}
    for key in QEMU_RATE_KEYS:
        values = [sample[key] for sample in samples if sample.get(key) is not None]
        medians[key] = statistics.median(values) if values else None
    return medians


def run_milestone_suite(
        engine, executable, flash, names, runs, warmup, quiet=False,
        extra_args=(), timeout_s=180.0, qemu_supervisor=None):
    results = []
    with tempfile.TemporaryDirectory(
            prefix="cemu-benchmark-%s-" % engine) as runtime:
        for name in names:
            for index in range(warmup):
                if not quiet:
                    print(
                        "%s %s warmup %d/%d" %
                        (engine, name, index + 1, warmup), file=sys.stderr
                    )
                run_ui_milestone(
                    engine, executable, flash, name, runtime,
                    extra_args=extra_args, timeout_s=timeout_s,
                    qemu_supervisor=qemu_supervisor,
                )
            samples = []
            for index in range(runs):
                if not quiet:
                    print(
                        "%s %s run %d/%d" %
                        (engine, name, index + 1, runs), file=sys.stderr
                    )
                samples.append(run_ui_milestone(
                    engine, executable, flash, name, runtime,
                    extra_args=extra_args, timeout_s=timeout_s,
                    qemu_supervisor=qemu_supervisor,
                ))
            results.append({
                "name": name,
                "description": SCENARIOS[name]["description"],
                "median": aggregate_milestone_samples(samples),
                "samples": samples,
            })
    return results


def cross_engine_ratios(engines):
    cemu = {
        item["name"]: item
        for item in engines.get("cemu", {}).get("scenarios", [])
    }
    qemu = {
        item["name"]: item
        for item in engines.get("qemu", {}).get("scenarios", [])
    }
    ratios = []
    for name in sorted(set(cemu).intersection(qemu)):
        values = {}
        for key in MILESTONE_MEDIAN_KEYS:
            denominator = cemu[name]["median"].get(key)
            numerator = qemu[name]["median"].get(key)
            values[key] = (
                numerator / denominator
                if numerator is not None and denominator not in (None, 0)
                else None
            )
        ratios.append({"name": name, "qemu_over_cemu": values})
    return ratios


def _sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        while True:
            chunk = handle.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def validate_ui_flash(path):
    actual = _sha256_file(path)
    if actual != CANONICAL_C55_SHA256:
        raise BenchmarkError(
            "UI milestones require canonical C55 SW24 SHA-256 %s, got %s" %
            (CANONICAL_C55_SHA256, actual)
        )


def validate_qemu_release_profile(executable):
    binary = Path(executable).resolve()
    build_dir = binary.parent
    build_root = build_dir.parent
    if binary.name != "qemu-system-c166" or build_dir.name != "release":
        raise BenchmarkError(
            "QEMU throughput requires qemu/build/release/qemu-system-c166"
        )
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise BenchmarkError("QEMU release binary is missing or not executable")
    entries = sorted(path.name for path in build_root.iterdir())
    if entries != ["debug", "release"]:
        raise BenchmarkError(
            "qemu/build must contain exactly debug/ and release/"
        )
    options_path = build_dir / "meson-info" / "intro-buildoptions.json"
    try:
        options = json.loads(options_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise BenchmarkError(
            "cannot read release Meson options: %s" % exc
        )
    actual = {option["name"]: option["value"] for option in options}
    expected = {
        "optimization": "3",
        "debug": False,
        "b_lto": True,
        "b_ndebug": "false",
        "qom_cast_debug": False,
        "debug_tcg": False,
        "debug_mutex": False,
        "debug_graph_lock": False,
        "b_pgo": "off",
        "c_args": [],
    }
    mismatches = [
        "%s=%r" % (name, actual.get(name))
        for name, wanted in expected.items() if actual.get(name) != wanted
    ]
    if mismatches:
        raise BenchmarkError(
            "QEMU release profile mismatch: " + ", ".join(mismatches)
        )
    return {name: actual[name] for name in expected}


def run_qemu_throughput_suite(
        executable, flash, names, runs, warmup, quiet=False,
        duration_s=QEMU_THROUGHPUT_WINDOW_S,
        warmup_ticks=QEMU_THROUGHPUT_WARMUP_TICKS):
    results = []
    with tempfile.TemporaryDirectory(
            prefix="qemu-throughput-") as runtime:
        for name in names:
            for index in range(warmup):
                if not quiet:
                    print(
                        "%s warmup %d/%d" % (name, index + 1, warmup),
                        file=sys.stderr,
                    )
                run_qemu_throughput(
                    executable, flash, name, runtime,
                    duration_s=duration_s, warmup_ticks=warmup_ticks,
                )
            samples = []
            for index in range(runs):
                if not quiet:
                    print(
                        "%s run %d/%d" % (name, index + 1, runs),
                        file=sys.stderr,
                    )
                samples.append(run_qemu_throughput(
                    executable, flash, name, runtime,
                    duration_s=duration_s, warmup_ticks=warmup_ticks,
                ))
            results.append({
                "name": name,
                "description": SCENARIOS[name]["description"],
                "sim": SCENARIOS[name]["sim"],
                "median": aggregate_qemu_samples(samples),
                "samples": samples,
            })
    return results


def evaluate_qemu_throughput(scenarios, baselines=None, minimum_no_sim=None):
    if baselines is None:
        return {"status": "not-qualified", "passed": None, "checks": []}
    by_name = {scenario["name"]: scenario for scenario in scenarios}
    checks = []
    no_sim = by_name["qemu-no-sim-throughput"]["median"][
        "process_wall_ticks_per_s"
    ]
    if minimum_no_sim is not None:
        checks.append({
            "name": "no-sim-absolute-minimum",
            "actual": no_sim,
            "minimum": minimum_no_sim,
            "passed": no_sim >= minimum_no_sim,
        })
    for name in QEMU_THROUGHPUT_SCENARIOS:
        actual = by_name[name]["median"]["process_wall_ticks_per_s"]
        minimum = baselines[name] * 0.9
        checks.append({
            "name": name + "-qualification-floor",
            "actual": actual,
            "baseline": baselines[name],
            "minimum": minimum,
            "passed": actual >= minimum,
        })
    return {
        "passed": all(check["passed"] for check in checks),
        "checks": checks,
    }


def build_qemu_throughput_report(
        executable, flash, names, runs, warmup, affinity, quiet=False):
    release_options = validate_qemu_release_profile(executable)
    scenarios = run_qemu_throughput_suite(
        executable, flash, names, runs, warmup, quiet=quiet,
    )
    return {
        "schema": QEMU_THROUGHPUT_SCHEMA,
        "generated_at": datetime.datetime.now(
            datetime.timezone.utc
        ).isoformat(),
        "host": {
            "platform": platform.platform(),
            "python": platform.python_version(),
            "cpu_count": os.cpu_count(),
            "affinity": affinity,
        },
        "configuration": {
            "qemu": executable,
            "supervisor": str(EMU_RUNNER),
            "flash": flash,
            "flash_sha256": CANONICAL_C55_SHA256,
            "runs": runs,
            "warmup": warmup,
            "warmup_ticks": QEMU_THROUGHPUT_WARMUP_TICKS,
            "window_s": QEMU_THROUGHPUT_WINDOW_S,
            "release_options": release_options,
            "qualification_baselines": QEMU_QUALIFICATION_BASELINES,
        },
        "scenarios": scenarios,
        "gate": evaluate_qemu_throughput(scenarios),
    }


def build_cross_engine_report(
        cemu, qemu, flash, names, runs, warmup, affinity, quiet=False,
        cemu_args=(), qemu_args=(), timeout_s=180.0):
    engines = {
        "cemu": {
            "configuration": {
                "executable": cemu,
                "arguments": list(cemu_args),
                "device": "c55",
                "fsn": CEMU_FSN,
            },
            "scenarios": run_milestone_suite(
                "cemu", cemu, flash, names, runs, warmup, quiet=quiet,
                extra_args=cemu_args, timeout_s=timeout_s
            ),
        },
    }
    if qemu is not None:
        engines["qemu"] = {
            "configuration": {
                "executable": qemu,
                "arguments": list(qemu_args),
                "supervisor": str(EMU_RUNNER),
                "machine": "siemens-c55",
                "fsn": CEMU_FSN,
                "icount": "shift=0,align=off,sleep=off",
            },
            "scenarios": run_milestone_suite(
                "qemu", qemu, flash, names, runs, warmup, quiet=quiet,
                extra_args=qemu_args, timeout_s=timeout_s,
                qemu_supervisor=EMU_RUNNER,
            ),
        }
    return {
        "schema": CROSS_ENGINE_SCHEMA,
        "generated_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "host": {
            "platform": platform.platform(),
            "python": platform.python_version(),
            "cpu_count": os.cpu_count(),
            "affinity": affinity,
        },
        "configuration": {
            "flash": flash,
            "flash_sha256": CANONICAL_C55_SHA256,
            "runs": runs,
            "warmup": warmup,
            "timeout_s": timeout_s,
        },
        "engines": engines,
        "ratios": cross_engine_ratios(engines),
    }


def compare_reports(current, baseline):
    old_by_name = {item["name"]: item for item in baseline.get("scenarios", [])}
    comparisons = []
    for scenario in current.get("scenarios", []):
        old = old_by_name.get(scenario["name"])
        if old is None:
            continue
        deltas = {}
        for key in RATE_KEYS:
            new_value = scenario.get("median", {}).get(key)
            old_value = old.get("median", {}).get(key)
            if new_value is None or old_value in (None, 0):
                deltas[key] = None
            else:
                deltas[key] = (new_value / old_value - 1.0) * 100.0
        comparisons.append({"name": scenario["name"], "median_delta_percent": deltas})
    return comparisons


def scenario_command(cemu, flash, name, snapshot=None, cemu_args=()):
    if name == "post-boot":
        if snapshot is None:
            raise BenchmarkError("post-boot scenario requires a prepared snapshot")
        command = [cemu, "--from-snapshot", str(snapshot)]
    else:
        command = [cemu, flash]
    return (
        command + SCENARIOS[name]["arguments"] + list(cemu_args) +
        ["--benchmark-json"]
    )


def prepare_post_boot_snapshot(cemu, flash, runtime_dir, cemu_args=()):
    label = "benchmark-post-boot-seed"
    command = [
        cemu, flash, "--limit", "20m",
    ] + list(cemu_args) + [
        "--snapshot", "--label", label, "--benchmark-json",
    ]
    run_cemu(command, cwd=runtime_dir)
    snapshot = Path(runtime_dir) / "shots" / label / "snapshot"
    if not (snapshot / "snapshot.json").is_file():
        raise BenchmarkError("cemu did not create the post-boot seed snapshot")
    return snapshot


def run_suite(
        cemu, cemu_inst, flash, names, runs, warmup, quiet=False, cemu_args=(), ui_limit=5000000):
    results = []
    with tempfile.TemporaryDirectory(prefix="cemu-benchmark-") as runtime:
        snapshot = None
        if "post-boot" in names:
            if not quiet:
                print("preparing post-boot snapshot", file=sys.stderr)
            snapshot = prepare_post_boot_snapshot(
                cemu_inst, flash, runtime, cemu_args
            )

        for name in names:
            scenario_cemu = cemu_inst if SCENARIOS[name].get("instrumented") else cemu
            arguments = (["--limit", str(ui_limit)] if name == "ui-connected"
                         else SCENARIOS[name]["arguments"])
            command = None
            if name != "ui-connected":
                command = scenario_command(scenario_cemu, flash, name, snapshot, cemu_args)
            warmup_samples = []
            for index in range(warmup):
                if not quiet:
                    print("%s warmup %d/%d" % (name, index + 1, warmup), file=sys.stderr)
                if name == "ui-connected":
                    warmup_samples.append(run_connected_ui(
                        cemu, flash, arguments, runtime, cemu_args
                    ))
                else:
                    warmup_samples.append(run_cemu(command, cwd=runtime))
            samples = []
            for index in range(runs):
                if not quiet:
                    print("%s run %d/%d" % (name, index + 1, runs), file=sys.stderr)
                if name == "ui-connected":
                    samples.append(run_connected_ui(
                        cemu, flash, arguments, runtime, cemu_args
                    ))
                else:
                    samples.append(run_cemu(command, cwd=runtime))
            results.append({
                "name": name,
                "description": SCENARIOS[name]["description"],
                "arguments": list(arguments),
                "median": aggregate_samples(samples),
                "samples": samples,
                "warmup_samples": warmup_samples,
            })
    return results


@contextlib.contextmanager
def pinned_cpu(cpu):
    if cpu is None:
        affinity = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None
        yield affinity
        return
    if not hasattr(os, "sched_setaffinity") or not hasattr(os, "sched_getaffinity"):
        raise BenchmarkError("CPU affinity is not supported on this platform")
    previous = os.sched_getaffinity(0)
    if cpu not in previous:
        raise BenchmarkError("CPU %d is outside the current affinity set" % cpu)
    os.sched_setaffinity(0, {cpu})
    try:
        yield [cpu]
    finally:
        os.sched_setaffinity(0, previous)


def build_report(
        cemu, cemu_inst, flash, names, runs, warmup, affinity,
        quiet=False, cemu_args=(), ui_limit=5000000):
    return {
        "schema": SCHEMA,
        "generated_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "host": {
            "platform": platform.platform(),
            "python": platform.python_version(),
            "cpu_count": os.cpu_count(),
            "affinity": affinity,
        },
        "configuration": {
            "cemu": cemu,
            "cemu_inst": cemu_inst,
            "flash": flash,
            "runs": runs,
            "warmup": warmup,
            "cemu_args": list(cemu_args),
            "ui_limit": ui_limit,
        },
        "scenarios": run_suite(
            cemu, cemu_inst, flash, names, runs, warmup, quiet=quiet,
            cemu_args=cemu_args, ui_limit=ui_limit
        ),
    }


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cemu", default="./bin/cemu", help="cemu executable")
    parser.add_argument(
        "--cemu-inst",
        help="instrumented executable (default: cemu sibling named cemu_inst)",
    )
    parser.add_argument(
        "--qemu", help="QEMU executable supervised by emu; enables cross-engine UI milestones"
    )
    parser.add_argument("--flash", required=True, help="full-flash image")
    parser.add_argument("--runs", type=int, default=3, help="measured runs per scenario")
    parser.add_argument("--warmup", type=int, default=1, help="discarded warmups per scenario")
    parser.add_argument("--cpu", type=int, help="pin the runner and cemu children to this CPU")
    parser.add_argument("--output", help="also write the JSON report to this path")
    parser.add_argument("--compare", help="baseline JSON report to compare against")
    parser.add_argument(
        "--cemu-arg", action="append", default=[], metavar="ARG",
        help="extra argument passed to every cemu invocation (repeatable; use = for flags)",
    )
    parser.add_argument(
        "--qemu-arg", action="append", default=[], metavar="ARG",
        help="extra argument passed to every supervised emu QEMU run (repeatable; use = for flags)",
    )
    parser.add_argument(
        "--ui-timeout", type=float, default=180.0,
        help="seconds allowed for each UI-protocol milestone",
    )
    parser.add_argument(
        "--scenario", action="append", choices=tuple(SCENARIOS),
        help="scenario to run; repeat as needed (default: all)",
    )
    parser.add_argument("--ui-limit", type=parse_count, default=5000000,
                        help="connected-UI tick limit (default: 5m)")
    parser.add_argument("--quiet", action="store_true", help="suppress progress on stderr")
    args = parser.parse_args(argv)
    if args.ui_limit < 1:
        parser.error("--ui-limit must be positive")
    if args.runs < 1:
        parser.error("--runs must be at least 1")
    if args.warmup < 0:
        parser.error("--warmup cannot be negative")
    if args.ui_timeout <= 0:
        parser.error("--ui-timeout must be positive")
    selected = args.scenario or (list(UI_SCENARIOS) if args.qemu else None)
    if selected and any(name in QEMU_THROUGHPUT_SCENARIOS for name in selected):
        if not args.qemu or any(
                name not in QEMU_THROUGHPUT_SCENARIOS for name in selected):
            parser.error("QEMU throughput scenarios cannot be mixed")
        if args.runs != 7 or args.warmup != 1:
            parser.error("QEMU throughput requires --warmup 1 --runs 7")
        if args.cpu is None:
            parser.error("QEMU throughput requires --cpu")
        if args.qemu_arg:
            parser.error("QEMU throughput rejects extra QEMU arguments")
    elif selected and any(name in UI_SCENARIOS for name in selected):
        if any(name not in UI_SCENARIOS for name in selected):
            parser.error("UI milestone and legacy tick scenarios cannot be mixed")
    elif args.qemu:
        parser.error("--qemu supports only ui-boot and ui-softkey scenarios")
    return args


def main(argv=None):
    args = parse_args(argv)
    cemu = str(Path(args.cemu).resolve())
    if args.cemu_inst:
        cemu_inst = str(Path(args.cemu_inst).resolve())
    else:
        cemu_inst = str(Path(cemu).with_name("cemu_inst"))
    qemu = str(Path(args.qemu).resolve()) if args.qemu else None
    flash = str(Path(args.flash).resolve())
    names = args.scenario or (list(UI_SCENARIOS) if qemu else list(LEGACY_SCENARIOS))
    try:
        with pinned_cpu(args.cpu) as affinity:
            if all(name in QEMU_THROUGHPUT_SCENARIOS for name in names):
                validate_ui_flash(flash)
                report = build_qemu_throughput_report(
                    qemu, flash, names, args.runs, args.warmup,
                    affinity, quiet=args.quiet,
                )
            elif all(name in UI_SCENARIOS for name in names):
                if any(SCENARIOS[name]["target_digest"] is None for name in names):
                    raise BenchmarkError("bundled C55 UI milestones are unqualified; use ui-connected or throughput scenarios")
                validate_ui_flash(flash)
                report = build_cross_engine_report(
                    cemu, qemu, flash, names, args.runs, args.warmup,
                    affinity, quiet=args.quiet, cemu_args=args.cemu_arg,
                    qemu_args=args.qemu_arg, timeout_s=args.ui_timeout
                )
            else:
                report = build_report(
                    cemu, cemu_inst, flash, names, args.runs, args.warmup,
                    affinity, quiet=args.quiet, cemu_args=args.cemu_arg,
                    ui_limit=args.ui_limit
                )
        if args.compare:
            if report["schema"] != SCHEMA:
                raise BenchmarkError(
                    "--compare currently accepts legacy schema-2 reports only"
                )
            else:
                with open(args.compare, "r", encoding="utf-8") as handle:
                    baseline = json.load(handle)
                report["comparison"] = compare_reports(report, baseline)
        rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
        if args.output:
            output = Path(args.output)
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(rendered, encoding="utf-8")
        sys.stdout.write(rendered)
        if report.get("schema") == QEMU_THROUGHPUT_SCHEMA:
            return 1 if report["gate"]["passed"] is False else 0
        return 0
    except (BenchmarkError, OSError, ValueError) as exc:
        print("benchmark: error: %s" % exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
