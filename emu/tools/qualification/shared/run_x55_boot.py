#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the versioned Siemens x55 QEMU/CEMU qualification matrix.

The stable EMU supervisor owns prepared storage and identity materialization.
This runner owns qualification policy, protocol-v13 frame/key milestones,
cross-engine effect normalization, and the retained matrix report.
"""

from __future__ import annotations

import argparse
import contextlib
import datetime as dt
import hashlib
import json
import os
import re
import shutil
import signal
import socket
import statistics
import struct
import subprocess
import sys
import time
import traceback
from pathlib import Path


# The supervised host owns the socket and its codec for both engines.
from tools.qualification.shared.support import repository_root, sha256_file, git_revision
from tools import ui_protocol

MATRIX_SCHEMA = "pmb7850-qemu-x55-matrix"
MATRIX_SCHEMA_VERSION = 2
REPORT_SCHEMA = "pmb7850-qemu-x55-matrix-report"
REPORT_SCHEMA_VERSION = 2
DEVICE_ORDER = (
    "a52", "a55", "a60", "a62", "a65", "c55",
    "c60", "cf62", "m55", "mc60", "s55", "sl55",
)
GATE_ORDER = ("perf", "no-sim", "sim", "confirm", "ui", "supervised")
TRACE_SELECTORS = (
    "serial,xbus_access,xbus_mailbox_complete,pec,"
    "keypad_command,keypad_result,keypad_input,tdma_irq,"
    "lcd_select,lcd_command,lcd_reset,lcd_frame,lcd_transaction,"
    "lifecycle,flash,sim"
)

UI_MAGIC = 0x55353543
UI_VERSION = ui_protocol.VERSION
UI_HEADER = ui_protocol.HEADER
UI_HELLO = 1
UI_FRAME = 2
UI_STATS = 3
UI_KEY = 4
UI_KEY_RELEASE_AFTER_SAMPLE = 6
UI_STATS_PAYLOAD = ui_protocol.STATS_PAYLOAD
UI_MAX_PAYLOAD = ui_protocol.MAX_PAYLOAD

SUMMARY_RE = re.compile(
    r"status:\s+(\S+)\s+ticks:\s+(\d+)\s+icount:\s+(\d+)\s+"
    r"pc:\s+(?:0x)?([0-9a-fA-F]+)"
)

EFFECT_GROUPS = {
    "flash": ("flash_",),
    "xbus": ("xbus_", "bus_window"),
    "pec_irq": ("pec_transfer", "tdma_irq"),
    "timer": ("tdma_tick",),
    "lcd_transaction": ("lcd_transaction",),
    "display": ("lcd_", "ssc0_"),
    "keypad": ("keypad_", "ef_mailbox_"),
    "serial": ("serial_",),
    "sim": ("sim_",),
}
INFO_KEYS = {
    "flash": ("mode", "operation", "operation_offset", "partition"),
    "xbus": ("access", "irq", "result", "status_before"),
    "pec_irq": ("channel", "count_after", "count_before", "dst", "src"),
    "timer": ("compare", "trap", "top"),
    "lcd_transaction": ("data_bytes", "disposition", "sequence", "x", "y"),
    "display": ("bank", "command", "data_bytes", "sequence", "x", "y",
                "bits", "rx", "tx"),
    "keypad": ("acknowledged", "column", "handled", "matrix", "pressed",
               "startup"),
    "serial": (),
    "sim": ("already_pending", "before", "cla", "direction", "ins", "p1",
            "p2", "p3", "response_length", "sequence", "status_word"),
}


class MatrixError(RuntimeError):
    pass


def default_manifest() -> Path:
    return repository_root() / "firmware/manifest.json"


def load_manifest(path: Path, root: Path, selected: list[str] | None = None,
                  include_alternates: bool = False) -> dict:
    text = path.read_text(encoding="utf-8")
    if text.startswith("// SPDX-License-Identifier:"):
        text = text.split("\n", 1)[1]
    value = json.loads(text)
    if value.get("schema") != MATRIX_SCHEMA or \
       value.get("schema_version") != MATRIX_SCHEMA_VERSION:
        raise MatrixError(f"unsupported matrix manifest: {path}")
    if value.get("synchronization_unit") != "soc_ticks":
        raise MatrixError("matrix synchronization unit must be soc_ticks")
    devices = value.get("devices")
    if not isinstance(devices, dict) or tuple(devices) != DEVICE_ORDER:
        raise MatrixError(
            "matrix devices are missing or out of canonical order"
        )
    # Optional corpus inputs must not prevent an unrelated focused gate.
    # Every selected variant still receives the complete provenance check.
    for name in DEVICE_ORDER if selected is None else selected:
        for variant_name, entry in variants_for(
                name, devices[name], include_alternates):
            validate_image_entry(f"{name}:{variant_name}", entry, root)
    return value


def validate_image_entry(name: str, entry: dict, root: Path) -> None:
    image = root / entry["image"]
    if not image.is_file():
        raise MatrixError(f"{name} image is missing: {image}")
    actual = sha256_file(image)
    if actual != entry["sha256"]:
        raise MatrixError(
            f"{name} SHA-256 mismatch: expected {entry['sha256']}, got {actual}"
        )
    if "size" in entry and image.stat().st_size != entry["size"]:
        raise MatrixError(f"{name} size does not match the manifest")
    topology = entry.get("topology")
    if not topology or \
       sum(chip["size"] for chip in topology) != image.stat().st_size:
        raise MatrixError(f"{name} topology does not cover the complete image")


def parse_selection(values: list[str] | None, allowed: tuple[str, ...],
                    what: str) -> list[str]:
    if not values:
        return list(allowed)
    selected: list[str] = []
    for value in values:
        for item in value.split(","):
            item = item.strip().lower()
            if item == "all":
                for candidate in allowed:
                    if candidate not in selected:
                        selected.append(candidate)
            elif item not in allowed:
                raise MatrixError(f"unknown {what}: {item}")
            elif item not in selected:
                selected.append(item)
    return selected


def identity_arguments(entry: dict, root: Path) -> list[str]:
    identity = entry["identity"]
    if identity["kind"] == "fsn":
        return ["--fsn", identity["value"]]
    if identity["kind"] == "overlay":
        return ["--eeprom-overlay", str((root / identity["path"]).resolve())]
    raise MatrixError(f"unsupported identity kind: {identity['kind']}")


def encode_ui(message_type: int, payload: bytes = b"",
              sequence: int = 0, icount: int = 0, *, run_id=bytes(16)) -> bytes:
    return ui_protocol.encode_packet(message_type, payload, sequence=sequence,
                                     icount=icount, run_id=run_id)


def decode_ui(buffer: bytes, identity=None
              ) -> tuple[list[tuple[int, bytes, int, int]], bytes]:
    try:
        packets, rest = ui_protocol.decode_packets(buffer)
        for packet in packets:
            if identity is not None:
                ui_protocol.observe_packet(packet, identity)
            if packet.message_type == ui_protocol.LIFECYCLE:
                ui_protocol.decode_lifecycle(packet.payload)
            elif packet.message_type == UI_STATS:
                ui_protocol.decode_stats(packet.payload)
        values = [(p.message_type, p.payload, p.sequence, p.icount)
                  for p in packets]
        return values, rest
    except ui_protocol.ProtocolError as exc:
        raise MatrixError(str(exc)) from exc


def decode_hello(payload: bytes) -> dict:
    try:
        width, height, model, keys, asc0, audio = \
            ui_protocol.decode_hello(payload)
    except ui_protocol.ProtocolError as exc:
        raise MatrixError(str(exc)) from exc
    return {"width": width, "height": height, "model": model,
            "keys": list(keys),
            "capabilities": (ui_protocol.CAP_ASC0 if asc0 else 0) |
                            (ui_protocol.CAP_AUDIO if audio else 0)}


def connect_ui(process: subprocess.Popen, path: Path,
               deadline: float) -> socket.socket:
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise MatrixError(
                f"supervisor exited before UI attach ({process.returncode})"
            )
        client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            client.connect(str(path))
            client.settimeout(0.25)
            return client
        except (FileNotFoundError, ConnectionRefusedError):
            client.close()
            time.sleep(0.01)
    raise MatrixError(f"UI socket did not appear: {path}")


def prepare_run_directory(path: Path) -> None:
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True)


def frame_hashes(run_dir: Path) -> list[str]:
    frames = sorted((run_dir / "shots" / "run" / "lcd").glob("frame-*.png"))
    return [sha256_file(frame) for frame in frames]


def parse_summary(text: str) -> dict:
    matches = list(SUMMARY_RE.finditer(text))
    if not matches:
        raise MatrixError("supervisor did not print a final runtime summary")
    status, ticks, icount, pc = matches[-1].groups()
    return {"status": status, "ticks": int(ticks),
            "guest_instructions": int(icount),
            "pc": int(pc, 16)}


def parse_benchmark(text: str) -> dict:
    for line in reversed(text.splitlines()):
        with contextlib.suppress(json.JSONDecodeError):
            value = json.loads(line)
            if isinstance(value, dict) and value.get("schema") == 2:
                return value
    raise MatrixError("CEMU did not print benchmark JSON")


def run_protocol_process(command: list[str], run_dir: Path, target: int,
                         timeout: float, *, stop_at_target: bool,
                         stop_pc: int | None = None,
                         stop_pc_grace_s: float = 0.0,
                         trigger_frame: int | None = None,
                         response_frame: int | None = None,
                         key: str | None = None,
                         key_presses: list[list] | None = None) -> dict:
    socket_path = Path("/tmp") / (
        f"pmb7850-x55-ui-{os.getpid()}-{time.monotonic_ns()}.sock")
    stdout_path = run_dir / "stdout.log"
    stderr_path = run_dir / "stderr.log"
    started = time.monotonic()
    with stdout_path.open("w", encoding="utf-8") as stdout, \
         stderr_path.open("w", encoding="utf-8") as stderr:
        process = subprocess.Popen(command + ["--ui-socket", str(socket_path)],
                                   cwd=run_dir, stdout=stdout, stderr=stderr,
                                   text=True, start_new_session=True)
        hello = None
        stats = {"ticks": 0, "guest_instructions": 0, "pc": 0}
        frames: list[str] = []
        sent_frame_key = False
        schedule = list(key_presses or [])
        schedule_index = 0
        buffer = b""
        identity = {}
        deadline = time.monotonic() + timeout
        target_reached_at = None
        target_stats = None
        target_wall_time_s = None
        client = None
        try:
            client = connect_ui(process, socket_path, deadline)
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    break
                try:
                    chunk = client.recv(1024 * 1024)
                    if not chunk:
                        break
                    buffer += chunk
                except socket.timeout:
                    pass
                except ConnectionResetError:
                    break
                packets, buffer = decode_ui(buffer, identity)
                for kind, payload, unused_sequence, unused_icount in packets:
                    del unused_sequence, unused_icount
                    if kind == UI_HELLO:
                        hello = decode_hello(payload)
                    elif kind == UI_STATS:
                        if len(payload) != UI_STATS_PAYLOAD.size:
                            raise MatrixError("invalid protocol-v13 STATS")
                        unused_elapsed, ticks, guest, pc = \
                            ui_protocol.decode_stats(payload)[:4]
                        del unused_elapsed
                        stats = {"ticks": ticks,
                                 "guest_instructions": guest, "pc": pc}
                    elif kind == UI_FRAME:
                        frames.append(hashlib.sha256(payload).hexdigest())
                if hello:
                    if trigger_frame is not None and not sent_frame_key and \
                       len(frames) >= trigger_frame:
                        if key not in hello["keys"]:
                            raise MatrixError(
                                f"{hello['model']} omits key {key}"
                            )
                        index = hello["keys"].index(key)
                        client.sendall(encode_ui(
                            UI_KEY, bytes((index, 1)),
                            run_id=identity["run_id"]))
                        client.sendall(encode_ui(
                            UI_KEY_RELEASE_AFTER_SAMPLE, bytes((index,)),
                            run_id=identity["run_id"]))
                        sent_frame_key = True
                    while schedule_index < len(schedule) and \
                          stats["ticks"] >= schedule[schedule_index][0]:
                        scheduled_key = schedule[schedule_index][1]
                        if scheduled_key not in hello["keys"]:
                            raise MatrixError(
                                f"{hello['model']} omits scheduled key "
                                f"{scheduled_key}"
                            )
                        index = hello["keys"].index(scheduled_key)
                        client.sendall(encode_ui(
                            UI_KEY, bytes((index, 1)),
                            run_id=identity["run_id"]))
                        client.sendall(encode_ui(
                            UI_KEY_RELEASE_AFTER_SAMPLE, bytes((index,)),
                            run_id=identity["run_id"]))
                        schedule_index += 1
                target_ready = stats["ticks"] >= target and \
                    schedule_index == len(schedule)
                if target_ready and target_reached_at is None:
                    target_reached_at = time.monotonic()
                    target_stats = dict(stats)
                    target_wall_time_s = target_reached_at - started
                reached = target_ready and \
                    (stop_pc is None or stats["pc"] == stop_pc)
                if response_frame is not None:
                    reached = reached or (sent_frame_key and
                                           len(frames) >= response_frame)
                grace_expired = stop_at_target and target_reached_at and \
                    stop_pc is not None and \
                    time.monotonic() - target_reached_at >= stop_pc_grace_s
                if (reached or grace_expired) and stop_at_target:
                    process.send_signal(signal.SIGINT)
                    break
            else:
                raise MatrixError(
                    f"runtime milestone timed out after {timeout:.1f}s"
                )
            if process.poll() is None:
                process.wait(timeout=15)
        finally:
            if client:
                client.close()
            if process.poll() is None:
                process.kill()
                process.wait()
            with contextlib.suppress(FileNotFoundError):
                socket_path.unlink()
    stdout_text = stdout_path.read_text(encoding="utf-8")
    stderr_text = stderr_path.read_text(encoding="utf-8")
    if process.returncode not in (0, 130):
        raise MatrixError(
            f"runtime exited {process.returncode}: "
            f"{stderr_text.strip() or stdout_text.strip()}"
        )
    return {"command": command, "hello": hello, "stats": stats,
            "protocol_frame_hashes": frames, "stdout": stdout_text,
            "stderr": stderr_text, "returncode": process.returncode,
            "wall_time_s": time.monotonic() - started,
            "target_stats": target_stats or stats,
            "target_wall_time_s": target_wall_time_s or
                                  time.monotonic() - started,
            "milestones": {
                "frame_key_sent": sent_frame_key,
                "response_frame_reached":
                    response_frame is None or len(frames) >= response_frame,
                "scheduled_keys_sent": schedule_index,
                "scheduled_keys_total": len(schedule),
                "stop_pc_reached": stop_pc is None or stats["pc"] == stop_pc,
            }}


def common_command(executable: Path, image: Path, device: str, entry: dict,
                   root: Path, sim: bool) -> list[str]:
    command = [str(executable), "run", str(image), "--device", device]
    command += identity_arguments(entry, root)
    if sim:
        command.append("--sim")
    return command


def gate_config(entry: dict, gate: str) -> dict:
    if gate in ("sim", "confirm"):
        return entry[gate]
    return entry["ui" if gate == "ui" else "no_sim"]


def run_qemu(args, device: str, entry: dict, gate: str,
             run_dir: Path, target: int) -> dict:
    image = (args.root / entry["image"]).resolve()
    command = common_command(args.emu, image, device, entry, args.root,
                             gate in ("sim", "confirm"))
    command += ["--engine", "qemu", "--qemu-binary", str(args.qemu),
                "--trace", TRACE_SELECTORS, "--lcd-frames", "--label", "run"]
    config = gate_config(entry, gate)
    return run_protocol_process(
        command, run_dir, target, args.timeout, stop_at_target=True,
        trigger_frame=config.get("trigger_frame"),
        response_frame=config.get("response_frame"),
        key=config.get("key"),
        key_presses=config.get("key_presses"),
    )


def run_cemu(args, device: str, entry: dict, gate: str,
             run_dir: Path, target: int) -> dict:
    image = (args.root / entry["image"]).resolve()
    command = [str(args.cemu), str(image), "--device", device]
    command += identity_arguments(entry, args.root)
    if gate in ("sim", "confirm"):
        command.append("--sim")
    command += ["--limit", str(target), "--trace", TRACE_SELECTORS,
                "--lcd-frames", "--label", "run", "--benchmark-json"]
    config = gate_config(entry, gate)
    stop_pc = int(config["expected_pc"], 0) \
        if config.get("expected_pc") else None
    if gate == "ui" or config.get("key_presses"):
        process_result = run_protocol_process(
            command, run_dir, target, args.timeout, stop_at_target=False,
            stop_pc=stop_pc,
            trigger_frame=config.get("trigger_frame"),
            response_frame=config.get("response_frame"),
            key=config.get("key"),
            key_presses=config.get("key_presses"),
        )
        benchmark = parse_benchmark(process_result["stdout"])
        return {**process_result, "benchmark": benchmark}
    completed = subprocess.run(command, cwd=run_dir, text=True,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               timeout=args.timeout)
    (run_dir / "stdout.log").write_text(completed.stdout, encoding="utf-8")
    (run_dir / "stderr.log").write_text(completed.stderr, encoding="utf-8")
    if completed.returncode:
        raise MatrixError(
            f"CEMU exited {completed.returncode}: "
            f"{completed.stderr.strip() or completed.stdout.strip()}"
        )
    return {"command": command, "stdout": completed.stdout,
            "stderr": completed.stderr, "returncode": completed.returncode,
            "benchmark": parse_benchmark(completed.stdout)}


def run_perf_engine(args, device: str, entry: dict, run_dir: Path,
                    target: int, *, qemu: bool,
                    stop_pc: int | None = None) -> dict:
    image = (args.root / entry["image"]).resolve()
    if qemu:
        command = common_command(args.emu, image, device, entry, args.root,
                                 False)
        command += ["--engine", "qemu", "--qemu-binary", str(args.qemu)]
        result = run_protocol_process(
            command, run_dir, target, 30.0, stop_at_target=True,
            stop_pc=stop_pc, stop_pc_grace_s=1.0)
        # The final supervisor summary is sampled after SIGINT and may catch
        # the CPU between idle-loop instructions.  The protocol STATS packet
        # is the synchronized observation that caused this run to stop.
        summary = parse_summary(result["stdout"])
        endpoint = {"status": summary["status"], **result["stats"]}
    else:
        command = [str(args.cemu), str(image), "--device", device]
        command += identity_arguments(entry, args.root)
        command += ["--limit", str(target), "--benchmark-json"]
        result = run_protocol_process(
            command, run_dir, target, 30.0, stop_at_target=False)
        benchmark = parse_benchmark(result["stdout"])
        endpoint = {
            "status": benchmark["status"],
            "ticks": benchmark["ticks"],
            "guest_instructions": benchmark["guest_instructions"],
            "pc": benchmark["pc"],
        }
        result["benchmark"] = benchmark
    measurement = result["target_stats"]
    wall = result["target_wall_time_s"]
    return {
        "command": compact_command(command),
        "endpoint": endpoint,
        "measurement": measurement,
        "wall_time_s": wall,
        "total_wall_time_s": result["wall_time_s"],
        "ticks_per_s": measurement["ticks"] / wall if wall else 0,
        "guest_instructions_per_s":
            measurement["guest_instructions"] / wall if wall else 0,
        "frames": result["protocol_frame_hashes"],
    }


def run_perf_gate(args, device: str, entry: dict, gate_dir: Path) -> dict:
    config = entry["no_sim"]
    requested = config.get("minimum_ticks", 20_000_000)
    minimum_frames = config.get("minimum_frames", 0)
    cemu_dir = gate_dir / "cemu-oracle"
    prepare_run_directory(cemu_dir)
    oracle = run_perf_engine(args, device, entry, cemu_dir, requested,
                             qemu=False)
    target = oracle["endpoint"]["ticks"]
    require_idle = oracle["endpoint"]["status"] == "idle"
    oracle_frame = (oracle["frames"][minimum_frames - 1]
                    if minimum_frames and
                    len(oracle["frames"]) >= minimum_frames else None)
    oracle_milestone = (len(oracle["frames"]) >= minimum_frames and
                        (not config.get("expected_pc") or
                         oracle["endpoint"]["pc"] ==
                         int(config["expected_pc"], 0)))

    warmup_dir = gate_dir / "qemu-warmup"
    prepare_run_directory(warmup_dir)
    warmup = run_perf_engine(args, device, entry, warmup_dir, target,
                             qemu=True, stop_pc=oracle["endpoint"]["pc"])
    samples = []
    for index in range(3):
        sample_dir = gate_dir / f"qemu-sample-{index + 1}"
        prepare_run_directory(sample_dir)
        sample = run_perf_engine(args, device, entry, sample_dir, target,
                                 qemu=True,
                                 stop_pc=oracle["endpoint"]["pc"])
        endpoint_match = sample["endpoint"]["pc"] == oracle["endpoint"]["pc"]
        frame_match = (oracle_frame is None or
                       (len(sample["frames"]) >= minimum_frames and
                        sample["frames"][minimum_frames - 1] == oracle_frame))
        idle_match = not require_idle or endpoint_match
        sample.update({"endpoint_match": endpoint_match,
                       "frame_milestone_match": frame_match,
                       "idle_match": idle_match})
        samples.append(sample)

    median_ticks = statistics.median(item["ticks_per_s"] for item in samples)
    median_guest = statistics.median(
        item["guest_instructions_per_s"] for item in samples)
    median_wall = statistics.median(item["wall_time_s"] for item in samples)
    previous = entry.get("performance_baseline_ticks_per_s")
    baseline_pass = previous is None or median_ticks >= previous * 0.9
    milestone_pass = oracle_milestone and all(
        item["endpoint_match"] and item["frame_milestone_match"] and
        item["idle_match"] for item in samples)
    passed = milestone_pass and baseline_pass
    return {
        "status": "pass" if passed else "fail",
        "wall_deadline_s": 30.0,
        "configuration": {"requested_ticks": requested,
                          "synchronized_ticks": target,
                          "minimum_frames": minimum_frames,
                          "idle_required": require_idle},
        "oracle": oracle,
        "warmup": warmup,
        "samples": samples,
        "median": {"ticks_per_s": median_ticks,
                   "guest_instructions_per_s": median_guest,
                   "wall_time_s": median_wall},
        "baseline": {"previous_ticks_per_s": previous,
                     "minimum_ticks_per_s": previous * 0.9
                         if previous is not None else None,
                     "candidate_ticks_per_s": median_ticks
                         if milestone_pass else None,
                     "passed": baseline_pass},
        "milestone": {"oracle_passed": oracle_milestone,
                      "passed": milestone_pass,
                      "frame_hash": oracle_frame,
                      "failure_reason": None if passed else
                          ("baseline-regression" if not baseline_pass else
                           "endpoint-frame-or-idle-mismatch")},
        "artifacts": str(gate_dir),
    }


def matching_group(kind: str) -> str | None:
    for group, prefixes in EFFECT_GROUPS.items():
        if any(kind == prefix or kind.startswith(prefix)
               for prefix in prefixes):
            return group
    return None


def read_effects(trace_root: Path) -> dict[str, list[tuple]]:
    try:
        import duckdb
    except ImportError as exc:
        raise MatrixError("matrix effect comparison requires duckdb") from exc
    parquet = str(trace_root / "kind=*" / "*.parquet")
    if not list(trace_root.glob("kind=*/*.parquet")):
        return {group: [] for group in EFFECT_GROUPS}
    connection = duckdb.connect()
    relation = connection.execute(
        "SELECT * FROM read_parquet(?, union_by_name=true, "
        "hive_partitioning=true) ORDER BY seq", [parquet]
    )
    columns = [item[0] for item in relation.description]
    groups = {group: [] for group in EFFECT_GROUPS}
    for values in relation.fetchall():
        row = dict(zip(columns, values))
        group = matching_group(row["kind"])
        if not group:
            continue
        normalized = [row["kind"], row.get("addr"), row.get("size"),
                      row.get("value")]
        for key in INFO_KEYS[group]:
            for suffix in ("_i64", "_bool", "_str"):
                column = f"info_{key}{suffix}"
                if column in row:
                    normalized.append(row[column])
                    break
            else:
                normalized.append(None)
        groups[group].append(tuple(normalized))
    connection.close()
    return groups


def first_difference(left: list, right: list) -> int | None:
    for index, (lvalue, rvalue) in enumerate(zip(left, right)):
        if lvalue != rvalue:
            return index
    return None if len(left) == len(right) else min(len(left), len(right))


def compare_effects(qemu_root: Path, cemu_root: Path) -> dict:
    qemu = read_effects(qemu_root)
    cemu = read_effects(cemu_root)
    report = {}
    for group in EFFECT_GROUPS:
        difference = first_difference(qemu[group], cemu[group])
        item = {"qemu_count": len(qemu[group]), "cemu_count": len(cemu[group]),
                "first_difference": difference}
        if difference is not None:
            item["qemu"] = qemu[group][difference] \
                if difference < len(qemu[group]) else None
            item["cemu"] = cemu[group][difference] \
                if difference < len(cemu[group]) else None
        report[group] = item
    report["sim"]["qemu_apdu_count"] = sum(
        effect[0] == "sim_apdu" for effect in qemu["sim"])
    report["sim"]["cemu_apdu_count"] = sum(
        effect[0] == "sim_apdu" for effect in cemu["sim"])
    return report


def compare_frames(qemu_dir: Path, cemu_dir: Path) -> dict:
    qemu = frame_hashes(qemu_dir)
    cemu = frame_hashes(cemu_dir)
    return {"qemu_count": len(qemu), "cemu_count": len(cemu),
            "first_difference": first_difference(qemu, cemu),
            "qemu_hashes": qemu, "cemu_hashes": cemu}


def compact_command(command: list[str]) -> list[str]:
    return [str(item) for item in command]


def legacy_c55_gate_parity(result: dict, gate: str) -> bool:
    if gate != "ui":
        return bool(result.get("parity"))
    differences = result.get("ui", {}).get("first_differences", {})
    return bool(differences) and all(
        value is None for value in differences.values()
    )


def run_supervised(args, device: str, entry: dict, run_dir: Path) -> dict:
    target = 1000000
    result = run_qemu(args, device, entry, "no-sim", run_dir, target)
    summary = parse_summary(result["stdout"])
    return {"command": compact_command(result["command"]),
            "hello": result["hello"], "endpoint": summary,
            "lifecycle": {"create": "pass", "start": "pass", "query": "pass",
                          "stop": "pass", "teardown": "pass"},
            "artifacts": str(run_dir)}


def run_confirmation(args, device: str, entry: dict, run_dir: Path) -> dict:
    config = entry["confirm"]
    target = config["minimum_ticks"]
    result = run_qemu(args, device, entry, "confirm", run_dir, target)
    endpoint = parse_summary(result["stdout"])
    hashes = frame_hashes(run_dir)
    required_frame = config["required_frame_sha256"]
    no_exit = "serial: EXIT:" not in result["stdout"]
    scheduled = result["milestones"]["scheduled_keys_sent"] == \
        result["milestones"]["scheduled_keys_total"]
    target_reached = result["stats"]["ticks"] >= target
    frame_reached = required_frame in hashes
    passed = no_exit and scheduled and target_reached and frame_reached
    return {
        "status": "pass" if passed else "fail",
        "command": compact_command(result["command"]),
        "configuration": config,
        "endpoint": endpoint,
        "milestones": result["milestones"],
        "serial_exit_absent": no_exit,
        "required_frame_reached": frame_reached,
        "frame_count": len(hashes),
        "artifacts": str(run_dir),
    }


def run_gate(args, device: str, variant_name: str, entry: dict,
             gate: str, gate_dir: Path) -> dict:
    prepare_run_directory(gate_dir)
    started = time.monotonic()
    if gate == "perf":
        result = run_perf_gate(args, device, entry, gate_dir)
        result["duration_s"] = time.monotonic() - started
        return result
    if gate == "confirm":
        result = run_confirmation(args, device, entry, gate_dir)
        result["duration_s"] = time.monotonic() - started
        if args.comparison_mode == "strict" and result["status"] == "fail":
            raise MatrixError(
                f"power-on confirmation failed for {device}:"
                f"{variant_name}"
            )
        return result

    if gate == "supervised":
        result = run_supervised(args, device, entry, gate_dir)
        result.update({
            "status": "pass", "duration_s": time.monotonic() - started
        })
        return result
    config = gate_config(entry, gate)
    requested = config.get("maximum_ticks",
                           config.get("minimum_ticks", 20000000))
    qemu_dir = gate_dir / "qemu"
    cemu_dir = gate_dir / "cemu"
    prepare_run_directory(qemu_dir)
    qemu = run_qemu(args, device, entry, gate, qemu_dir, requested)
    qemu_endpoint = parse_summary(qemu["stdout"])
    target = qemu_endpoint["ticks"]
    prepare_run_directory(cemu_dir)
    cemu = run_cemu(args, device, entry, gate, cemu_dir, target)
    cemu_endpoint = {
        "status": cemu["benchmark"]["status"],
        "ticks": cemu["benchmark"]["ticks"],
        "guest_instructions": cemu["benchmark"]["guest_instructions"],
        "pc": cemu["benchmark"]["pc"],
        "state_digest": cemu["benchmark"]["state_digest"],
    }
    effects = compare_effects(
        qemu_dir / "shots" / "run" / "trace" / "trace.parquet",
        cemu_dir / "shots" / "run" / "trace" / "trace.parquet",
    )
    frames = compare_frames(qemu_dir, cemu_dir)
    expected_pc = (int(config["expected_pc"], 0)
                   if config.get("expected_pc") else None)
    cpu_difference = qemu_endpoint["pc"] != cemu_endpoint["pc"]
    expected_difference = expected_pc is not None and \
        (qemu_endpoint["pc"] != expected_pc or
         cemu_endpoint["pc"] != expected_pc)
    differences = cpu_difference or expected_difference or \
        frames["first_difference"] is not None or \
        any(item["first_difference"] is not None for item in effects.values())
    minimum_frames = config.get("minimum_frames", 0)
    if len(frames["qemu_hashes"]) < minimum_frames or \
       len(frames["cemu_hashes"]) < minimum_frames:
        differences = True
    milestones = qemu["milestones"]
    if gate == "ui" and (not milestones["frame_key_sent"] or
                         not milestones["response_frame_reached"]):
        differences = True
    if gate == "sim" and \
       milestones["scheduled_keys_sent"] != milestones["scheduled_keys_total"]:
        differences = True
    minimum_apdus = config.get("minimum_apdus", 0)
    if effects["sim"]["qemu_apdu_count"] < minimum_apdus or \
       effects["sim"]["cemu_apdu_count"] < minimum_apdus:
        differences = True
    result = {
        "status": "pass" if not differences else "fail",
        "comparison_mode": args.comparison_mode,
        "variant": variant_name,
        "configuration": {"requested_ticks": requested,
                          "synchronized_ticks": target,
                          "expected_pc": expected_pc},
        "commands": {"qemu": compact_command(qemu["command"]),
                     "cemu": compact_command(cemu["command"])},
        "cpu": {"qemu": qemu_endpoint, "cemu": cemu_endpoint,
                "first_difference": "pc" if cpu_difference else None,
                "expected_endpoint_difference": expected_difference},
        "memory": {"status": "not-collected", "first_difference": None},
        "effects": effects,
        "frames": frames,
        "key": {"requested": config.get("key"),
                "release": ("after-firmware-sample"
                            if config.get("key") else None),
                **milestones},
        "artifacts": {"root": str(gate_dir), "qemu": str(qemu_dir),
                      "cemu": str(cemu_dir)},
        "duration_s": time.monotonic() - started,
    }
    if args.comparison_mode == "strict" and differences:
        raise MatrixError(
            f"strict parity failed for {device}:{variant_name} {gate}"
        )
    return result


def variants_for(device: str, entry: dict,
                 include_alternates: bool) -> list[tuple[str, dict]]:
    variants = [("canonical", entry)]
    if include_alternates:
        for alternate in entry.get("alternates", []):
            merged = dict(entry)
            merged.update(alternate)
            variants.append((alternate["name"], merged))
    return variants


def refresh_legacy_c55_ui(report: dict) -> None:
    ui = report.get("devices", {}).get("c55", {}).get(
        "canonical", {}).get("ui")
    if not ui or "legacy_result" not in ui:
        return
    ui["parity"] = legacy_c55_gate_parity(ui["legacy_result"], "ui")
    ui["status"] = "pass" if ui["parity"] else "fail"


def report_has_failed_gate(report: dict) -> bool:
    return any(
        gate.get("status") == "fail"
        for variants in report.get("devices", {}).values()
        for gates in variants.values()
        for gate in gates.values()
    )


def merge_reports(paths: list[Path], artifacts: Path) -> int:
    if not paths:
        raise MatrixError("at least one matrix report is required")
    reports = [json.loads(path.read_text(encoding="utf-8")) for path in paths]
    first = reports[0]
    if first.get("schema") != REPORT_SCHEMA or \
       first.get("schema_version") != REPORT_SCHEMA_VERSION:
        raise MatrixError(f"unsupported matrix report: {paths[0]}")
    merged = {
        **first,
        "created_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "status": "pass",
        "selection": {"devices": [], "gates": [],
                      "include_alternates": False},
        "devices": {},
        "merged_reports": [str(path.resolve()) for path in paths],
    }
    for path, report in zip(paths, reports):
        refresh_legacy_c55_ui(report)
        if report.get("schema") != REPORT_SCHEMA or \
           report.get("schema_version") != REPORT_SCHEMA_VERSION:
            raise MatrixError(f"unsupported matrix report: {path}")
        for key in ("comparison_mode", "manifest", "provenance"):
            if report.get(key) != first.get(key):
                raise MatrixError(f"matrix report {key} differs: {path}")
        for device in report["selection"]["devices"]:
            if device not in merged["devices"]:
                merged["selection"]["devices"].append(device)
                merged["devices"][device] = report["devices"][device]
                continue
            # A later focused rerun replaces only its reported gates.  This
            # preserves completed long workflows when one gate needs repair.
            for variant, gates in report["devices"][device].items():
                merged["devices"][device].setdefault(variant, {}).update(gates)
        for gate in report["selection"]["gates"]:
            if gate not in merged["selection"]["gates"]:
                merged["selection"]["gates"].append(gate)
        merged["selection"]["include_alternates"] |= \
            report["selection"]["include_alternates"]
    merged["selection"]["devices"].sort(key=DEVICE_ORDER.index)
    merged["selection"]["gates"].sort(key=GATE_ORDER.index)
    if report_has_failed_gate(merged):
        merged["status"] = "fail"
    artifacts.mkdir(parents=True, exist_ok=True)
    report_path = artifacts / "report.json"
    report_path.write_text(
        json.dumps(merged, indent=2, sort_keys=True) + "\n",
        encoding="utf-8"
    )
    print(f"{merged['status'].upper()} merged matrix report: {report_path}")
    return 0 if merged["status"] == "pass" else 1


def matrix_main(argv: list[str] | None = None) -> int:
    root = repository_root()
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, default=default_manifest())
    parser.add_argument("--device", action="append")
    parser.add_argument("--gate", action="append")
    parser.add_argument("--comparison-mode", choices=("strict", "probe"))
    parser.add_argument("--qemu", type=Path,
                        default=root / "qemu/build/release/qemu-system-c166")
    parser.add_argument("--cemu", type=Path, default=root / "emu/bin/cemu_inst")
    parser.add_argument("--emu", type=Path, default=root / "emu/bin/emu")
    parser.add_argument("--artifacts", type=Path,
                        default=(root / "emu/shots/"
                                 "qemu-phase6-device-crosscheck"))
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("--include-alternates", action="store_true")
    parser.add_argument("--preflight-only", action="store_true")
    parser.add_argument("--verify-images", action="store_true",
                        help="check selected image hashes and sizes without requiring engine binaries")
    parser.add_argument("--startup-smoke", action="store_true",
                        help="100,000-tick startup checks in both engines; no boot/UI/parity claim")
    parser.add_argument("--merge-report", action="append", type=Path)
    args = parser.parse_args(argv)
    args.root = root.resolve()
    args.manifest = args.manifest.resolve()
    args.qemu = args.qemu.resolve()
    args.cemu = args.cemu.resolve()
    args.emu = args.emu.resolve()
    args.artifacts = args.artifacts.resolve()
    if args.merge_report:
        try:
            return merge_reports(
                [path.resolve() for path in args.merge_report], args.artifacts
            )
        except MatrixError as exc:
            parser.error(str(exc))
    try:
        devices = parse_selection(args.device, DEVICE_ORDER, "device")
        manifest = load_manifest(args.manifest, args.root, devices,
                                 args.include_alternates)
        gates = parse_selection(
            args.gate or manifest.get("default_gates"),
            tuple(manifest.get("supported_gates", GATE_ORDER)), "gate")
        args.comparison_mode = (args.comparison_mode or
                                manifest.get("comparison_mode", "strict"))
        if args.verify_images:
            count = sum(len(variants_for(name, manifest["devices"][name],
                                         args.include_alternates))
                        for name in devices)
            print(f"PASS firmware verification: {count} images")
            return 0
        for executable in (args.qemu, args.cemu, args.emu):
            if not executable.is_file() or not os.access(executable, os.X_OK):
                raise MatrixError(f"executable is missing: {executable}")
    except MatrixError as exc:
        parser.error(str(exc))
    report = {
        "schema": REPORT_SCHEMA, "schema_version": REPORT_SCHEMA_VERSION,
        "status": "preflight" if args.preflight_only else "running",
        "created_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "comparison_mode": args.comparison_mode,
        "manifest": {"path": str(args.manifest),
                     "sha256": sha256_file(args.manifest)},
        "provenance": {"superproject_commit": git_revision(args.root),
                       "qemu_commit": git_revision(args.root, "qemu"),
                       "qemu": str(args.qemu), "cemu": str(args.cemu),
                       "emu": str(args.emu)},
        "selection": {"devices": devices, "gates": gates,
                      "include_alternates": args.include_alternates},
        "devices": {},
    }
    args.artifacts.mkdir(parents=True, exist_ok=True)
    report_path = args.artifacts / "report.json"
    if args.startup_smoke:
        report["schema"] = "pmb7850-bounded-startup-report"
        report["scope"] = "image loading and early execution only; boot, UI and parity not qualified"
        report["selection"]["gates"] = ["startup-smoke"]
        failed = False
        for device in devices:
            for variant_name, entry in variants_for(
                    device, manifest["devices"][device], args.include_alternates):
                results = report["devices"].setdefault(device, {}).setdefault(variant_name, {})
                results["image_sha256"] = entry["sha256"]
                for engine in ("cemu", "qemu"):
                    print(f"== {device}:{variant_name} {engine} startup ==", flush=True)
                    run_dir = args.artifacts / device / variant_name / engine
                    prepare_run_directory(run_dir)
                    try:
                        result = run_perf_engine(args, device, entry, run_dir, 100_000,
                                                 qemu=engine == "qemu")
                        passed = result["endpoint"]["ticks"] >= 100_000
                        result["status"] = "pass" if passed else "fail"
                        failed |= not passed
                        results[engine] = result
                    except Exception as exc:
                        failed = True
                        results[engine] = {"status": "fail", "error": str(exc)}
                    report["status"] = "fail" if failed else "running"
                    report_path.write_text(json.dumps(report, indent=2) + "\n")
        report["status"] = "fail" if failed else "pass"
        report_path.write_text(json.dumps(report, indent=2) + "\n")
        print(f"{report['status'].upper()} bounded startup report: {report_path}")
        return int(failed)
    if args.preflight_only:
        report["status"] = "pass"
        report_path.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n",
            encoding="utf-8"
        )
        print(f"PASS matrix preflight: {len(devices)} devices")
        return 0
    failed = False
    for device in devices:
        entry = manifest["devices"][device]
        device_report = report["devices"].setdefault(device, {})
        for variant_name, variant in variants_for(
                device, entry, args.include_alternates):
            variant_report = device_report.setdefault(variant_name, {})
            for gate in gates:
                if gate == "confirm" and "confirm" not in variant:
                    continue
                if variant_name != "canonical" and gate in (
                        "perf", "ui", "supervised"):
                    continue
                gate_dir = args.artifacts / device / variant_name / gate
                print(f"== {device}:{variant_name} {gate} ==", flush=True)
                try:
                    variant_report[gate] = run_gate(
                        args, device, variant_name, variant, gate, gate_dir)
                    if variant_report[gate].get("status") == "fail":
                        failed = True
                except Exception as exc:
                    failed = True
                    variant_report[gate] = {
                        "status": "fail",
                        "error": f"{type(exc).__name__}: {exc}",
                        "traceback": traceback.format_exc(),
                        "artifacts": str(gate_dir),
                    }
                    if args.comparison_mode == "strict":
                        report["status"] = "fail"
                        report_path.write_text(
                            json.dumps(report, indent=2, sort_keys=True) + "\n",
                            encoding="utf-8")
                        raise
                report_path.write_text(
                    json.dumps(report, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")
    report["status"] = "fail" if failed else "pass"
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                           encoding="utf-8")
    print(f"{report['status'].upper()} matrix report: {report_path}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(matrix_main())
