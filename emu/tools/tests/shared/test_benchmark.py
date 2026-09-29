#!/usr/bin/env python3

import hashlib
import json
import re
import subprocess
import stat
import struct
import sys
import tempfile
import unittest
from tools.bundled_firmware import image as bundled_image
from pathlib import Path
from unittest import mock

EMU_ROOT = Path(__file__).resolve().parents[3]
REPO_ROOT = EMU_ROOT.parent
sys.path.insert(0, str(EMU_ROOT))

from tools import benchmark


VALID_STATS = {
    "schema": 2,
    "status": "limit",
    "device": "c55",
    "start_icount": 0,
    "end_icount": 100,
    "ticks": 100,
    "guest_instructions": 75,
    "elapsed_s": 0.5,
    "ticks_per_s": 200.0,
    "guest_instructions_per_s": 150.0,
    "pc": 4660,
    "state_digest": "0123456789abcdef",
}


def rate_sample(value):
    return {key: value for key in benchmark.RATE_KEYS}


def write_executable(path, text):
    path.write_text(text, encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


def make_fake_cemu(path, log_path):
    script = "#!/usr/bin/env python3\nLOG = " + repr(str(log_path)) + "\n" + r'''
import json
import subprocess
import pathlib
import sys

args = sys.argv[1:]
with open(LOG, "a") as handle:
    handle.write(json.dumps(args) + "\n")

def count(value):
    suffix = value[-1:].lower()
    if suffix == "m":
        return int(value[:-1]) * 1000000
    if suffix == "k":
        return int(value[:-1]) * 1000
    return int(value, 0)

limit = count(args[args.index("--limit") + 1])
start = 20000000 if "--from-snapshot" in args else 0
if "--snapshot" in args:
    label = args[args.index("--label") + 1]
    snapshot = pathlib.Path.cwd() / "shots" / label / "snapshot"
    snapshot.mkdir(parents=True)
    (snapshot / "snapshot.json").write_text("{}")
stats = {
    "schema": 2,
    "status": "limit",
    "device": "c55",
    "start_icount": start,
    "end_icount": start + limit,
    "ticks": limit,
    "guest_instructions": limit // 2,
    "elapsed_s": 0.25,
    "ticks_per_s": limit / 0.25,
    "guest_instructions_per_s": (limit // 2) / 0.25,
    "pc": 4660,
    "state_digest": "0123456789abcdef",
}
print(json.dumps(stats, separators=(",", ":")))
'''
    write_executable(path, script)

def make_fake_ui_cemu(path):
    script = r'''#!/usr/bin/env python3
import json
import socket
import struct
import sys

HEADER = struct.Struct("<IHHIQQI16s")
STATS = struct.Struct("<QQQIQQQddI")
MAGIC = 0x55353543
VERSION = 13
args = sys.argv[1:]
path = args[args.index("--ui-socket") + 1]

def send(conn, kind, payload, sequence, icount):
    conn.sendall(HEADER.pack(MAGIC, VERSION, kind, len(payload), sequence, icount, 0, bytes(16)) + payload)

server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(path)
server.listen(1)
conn, _ = server.accept()
keys = (b"0", b"1", b"2", b"3", b"4", b"5", b"6", b"7", b"8", b"9",
        b"star", b"hash", b"up", b"down", b"soft-left", b"soft-right",
        b"send", b"power")
hello = bytearray(struct.pack("<HHHHB", 101, 64, len(keys), 0, 3) + b"c55")
for key in keys:
    hello.append(len(key))
    hello.extend(key)
send(conn, 1, bytes(hello), 1, 100)
send(conn, 3, STATS.pack(100_000_000, 100, 80, 0x800064, 1, 100_000_000, 0, 0., 0., 0), 2, 100)
send(conn, 2, bytes(101 * 64 * 3), 3, 100)
conn.close()
server.close()
limit_text = args[args.index("--limit") + 1]
if limit_text.lower().endswith("k"):
    limit = int(limit_text[:-1]) * 1000
elif limit_text.lower().endswith("m"):
    limit = int(limit_text[:-1]) * 1000000
else:
    limit = int(limit_text, 0)
print(json.dumps({
    "schema": 2,
    "status": "limit",
    "device": "c55",
    "start_icount": 0,
    "end_icount": limit,
    "ticks": limit,
    "guest_instructions": limit,
    "elapsed_s": 0.25,
    "ticks_per_s": limit / 0.25,
    "guest_instructions_per_s": limit / 0.25,
    "pc": 4660,
    "state_digest": "0123456789abcdef",
}))
'''
    write_executable(path, script)


def make_fake_milestone_engine(path, log_path, version=7):
    script = "#!/usr/bin/env python3\nLOG = " + repr(str(log_path)) + "\n" + r'''
import json
import socket
import struct
import sys

HEADER = struct.Struct("<IHHIQQI")
STATS = struct.Struct("<QQQI")
MAGIC = 0x55353543
VERSION = 9
args = sys.argv[1:]
path = args[args.index("--ui-socket") + 1]

def packet(kind, payload, sequence, icount):
    return HEADER.pack(MAGIC, VERSION, kind, len(payload), sequence, icount, 0) + payload

server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(path)
server.listen(1)
conn, _ = server.accept()
keys = (b"0", b"1", b"2", b"3", b"4", b"5", b"6", b"7", b"8", b"9",
        b"star", b"hash", b"up", b"down", b"soft-left", b"soft-right",
        b"send", b"power")
hello = bytearray(struct.pack("<HHHHB", 101, 64, len(keys), 0, 3) + b"c55")
for key in keys:
    hello.append(len(key))
    hello.extend(key)
conn.sendall(packet(1, bytes(hello), 1, 100))
conn.sendall(packet(3, STATS.pack(100_000_000, 100, 80, 0x012A06), 2, 100))
conn.sendall(packet(2, b"boot", 3, 100))
received = b""
while len(received) < 2 * HEADER.size + 3:
    chunk = conn.recv(4096)
    if not chunk:
        break
    received += chunk
types = []
offset = 0
while len(received) - offset >= HEADER.size:
    header = HEADER.unpack(received[offset:offset + HEADER.size])
    length = header[3]
    end = offset + HEADER.size + length
    if end > len(received):
        break
    assert header[1] == VERSION, header
    types.append(header[2])
    offset = end
with open(LOG, "w") as handle:
    json.dump({"args": args, "types": types}, handle)
conn.sendall(packet(3, STATS.pack(200_000_000, 1100, 880, 0x012A06), 4, 1100))
conn.sendall(packet(2, b"dialog", 5, 1100))
conn.recv(1)
'''
    script = script.replace("VERSION = 9", f"VERSION = {version}")
    if version == 13:
        script = script.replace('"<IHHIQQI"', '"<IHHIQQI16s"').replace('"<QQQI"', '"<QQQIQQQddI"')
        script = script.replace('sequence, icount, 0)', 'sequence, icount, 0, bytes(16))')
        script = script.replace('0x012A06)', '0x012A06, 1, 100, 0, 0., 0., 0)')
    write_executable(path, script)


def make_fake_throughput_engine(path):
    script = r'''#!/usr/bin/env python3
import socket
import struct
import sys
import time

HEADER = struct.Struct("<IHHIQQI16s")
STATS = struct.Struct("<QQQIQQQddI")
MAGIC = 0x55353543
VERSION = 13
args = sys.argv[1:]
socket_path = args[args.index("--ui-socket") + 1]

server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(socket_path)
server.listen(1)
conn, _ = server.accept()
hello = struct.pack("<HHHHB", 1, 1, 1, 0, 3) + b"c55\x05power"
conn.sendall(HEADER.pack(MAGIC, VERSION, 1, len(hello), 1, 0, 0, bytes(16)) + hello)
for index in range(20):
    ticks = 4_000_000 + index * 1_000_000
    payload = STATS.pack(index * 20_000_000, ticks, ticks // 2, 0x012A06, index + 1, index * 20_000_000, 0, 0., 0., 0)
    conn.sendall(HEADER.pack(
        MAGIC, VERSION, 3, len(payload), index + 2, ticks, 0, bytes(16)
    ) + payload)
    time.sleep(0.02)
conn.close()
server.close()
'''
    write_executable(path, script)



class DriverIntegrationTests(unittest.TestCase):
    cemu = EMU_ROOT / "bin" / "cemu"
    cemu_inst = EMU_ROOT / "bin" / "cemu_inst"
    flash = bundled_image("c55")

    def test_real_json_and_snapshot_resume_accounting(self):
        with tempfile.TemporaryDirectory(prefix="cemu-benchmark-driver-") as directory:
            first = subprocess.run([
                str(self.cemu_inst), str(self.flash), "--limit", "1k",
                "--snapshot", "--label", "seed", "--benchmark-json",
            ], cwd=directory, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
               universal_newlines=True)
            self.assertEqual(first.returncode, 0, first.stderr)
            initial = benchmark.parse_cemu_json(first.stdout)
            self.assertEqual((initial["start_icount"], initial["end_icount"], initial["ticks"]),
                             (0, 1000, 1000))

            plain_initial_run = subprocess.run([
                str(self.cemu), str(self.flash), "--limit", "1k", "--benchmark-json",
            ], cwd=directory, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
               universal_newlines=True)
            self.assertEqual(plain_initial_run.returncode, 0, plain_initial_run.stderr)
            plain_initial = benchmark.parse_cemu_json(plain_initial_run.stdout)
            self.assertEqual(
                plain_initial["state_digest"], initial["state_digest"]
            )

            snapshot = Path(directory) / "shots" / "seed" / "snapshot"
            second = subprocess.run([
                str(self.cemu), "--from-snapshot", str(snapshot), "--limit", "100", "--benchmark-json",
            ], cwd=directory, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
               universal_newlines=True)
            self.assertEqual(second.returncode, 0, second.stderr)
            resumed = benchmark.parse_cemu_json(second.stdout)
            self.assertEqual((resumed["start_icount"], resumed["end_icount"], resumed["ticks"]),
                             (1000, 1100, 100))
            invocations = [benchmark.run_connected_ui(
                str(self.cemu), str(self.flash),
                ["--from-snapshot", str(snapshot), "--limit", "1m"], directory)
                for _ in range(2)]
            self.assertNotEqual(invocations[0]["run_id"], invocations[1]["run_id"])
            for invocation in invocations:
                self.assertEqual(invocation["final_stats"]["start_icount"], 1000)
                self.assertEqual(invocation["final_stats"]["ticks"], 1000000)
                self.assertEqual(invocation["end_icount"], 1001000)
                self.assertEqual(invocation["stats_samples"][-1]["ticks"], 1000000)

    def test_real_v13_socket_attachment(self):
        benchmark.validate_ui_flash(self.flash)
        with tempfile.TemporaryDirectory(prefix="emu-benchmark-v13-") as directory:
            sample = benchmark.run_connected_ui(
                str(self.cemu), str(self.flash), ["--limit", "1m"], directory)
        self.assertEqual(sample["ui_protocol_version"], 13)
        self.assertEqual(sample["lifecycle"]["phase"], "stopped")
        self.assertEqual(sample["lifecycle"]["status"], "limit")
        self.assertNotEqual(sample["run_id"], "00" * 16)
        self.assertEqual(sample["stats_samples"][-1]["icount"], sample["end_icount"])
        self.assertEqual(sample["end_icount"], 1000000)
        self.assertGreater(len(sample["stats_samples"]), 1)
        self.assertEqual(sample["stats_intervals_ns"]["runtime_elapsed"]["count"],
                         sample["ui_messages"]["stats"] - 1)

    def test_plain_driver_does_not_recognize_summary(self):
        completed = subprocess.run(
            [str(self.cemu), "--benchmark-json", "--summary"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True,
        )
        self.assertEqual(completed.returncode, 2)
        self.assertIn("unknown argument: --summary", completed.stderr)


class ProtocolTests(unittest.TestCase):
    def test_parse_cemu_json(self):
        self.assertEqual(
            benchmark.parse_cemu_json(json.dumps(VALID_STATS)),
            VALID_STATS,
        )
        with self.assertRaises(benchmark.BenchmarkError):
            benchmark.parse_cemu_json("progress\n" + json.dumps(VALID_STATS))
        malformed = dict(VALID_STATS)
        del malformed["ticks"]
        with self.assertRaises(benchmark.BenchmarkError):
            benchmark.parse_cemu_json(json.dumps(malformed))

    def test_parse_args_collects_emulator_args(self):
        args = benchmark.parse_args([
            "--flash", "flash.bin",
            "--cemu-arg=--batch-idle",
            "--cemu-arg=--sim",
        ])
        self.assertEqual(args.cemu_arg, ["--batch-idle", "--sim"])

    def test_parse_args_qemu_defaults_to_ui_scenarios(self):
        args = benchmark.parse_args([
            "--flash", "flash.bin", "--qemu", "qemu-system-c166",
            "--qemu-arg=-d", "--qemu-arg=exec",
        ])
        self.assertEqual(args.qemu_arg, ["-d", "exec"])

    def test_qemu_throughput_cli_requires_qualification_shape(self):
        args = benchmark.parse_args([
            "--flash", "flash.bin", "--qemu", "qemu-system-c166",
            "--scenario", "qemu-no-sim-throughput",
            "--scenario", "qemu-sim-throughput",
            "--warmup", "1", "--runs", "7", "--cpu", "3",
        ])
        self.assertEqual(args.cpu, 3)
        with self.assertRaises(SystemExit):
            benchmark.parse_args([
                "--flash", "flash.bin", "--qemu", "qemu-system-c166",
                "--scenario", "qemu-no-sim-throughput",
            ])

    def test_ui_version_matches_host(self):
        header = (EMU_ROOT / "include/emu_ui.h").read_text()
        version = int(re.search(r"#define EMU_UI_VERSION (\d+)u", header)[1])
        self.assertEqual(benchmark.UI_VERSION, version)
        packet = benchmark.encode_ui_packet(benchmark.UI_STATS)
        self.assertEqual(benchmark.UI_HEADER.unpack(packet)[1], version)
    def test_explicit_versions_reject_cross_engine_and_unknown_headers(self):
        for version in (7, 13):
            packet = benchmark.encode_ui_packet(benchmark.UI_RELEASE_ALL, version=version)
            self.assertEqual(struct.unpack_from("<H", packet, 4)[0], version)
            self.assertEqual(len(benchmark.decode_ui_packets(packet, version=version)[0]), 1)
            with self.assertRaises(benchmark.BenchmarkError):
                benchmark.decode_ui_packets(packet, version=19-version)
        for retired_version in (8, 9, 10, 11):
            with self.assertRaises(benchmark.BenchmarkError):
                benchmark.decode_ui_packets(benchmark.UI_HEADER.pack(
                    benchmark.UI_MAGIC, retired_version, 1, 0, 0, 0, 0, bytes(16)))

    def test_ui_limit_and_interval_distributions(self):
        self.assertEqual(benchmark.parse_args(["--flash", "x"]).ui_limit, 5000000)
        self.assertEqual(benchmark.parse_args(["--flash", "x", "--ui-limit", "50m"]).ui_limit, 50000000)
        self.assertEqual(benchmark.interval_distribution([10, 20, 20, 50]),
                         {"count": 3, "min": 0, "median": 10, "p95": 30, "max": 30})
        self.assertIsNone(benchmark.interval_distribution([10])["median"])

    def test_cemu_hello_validation_matches_shared_codec_and_capture(self):
        from tools import ui_protocol as protocol
        from tools.info_menu_capture import CaptureError, decode_hello
        valid = struct.pack("<HHHHB", 1, 1, 1, 3, 3) + b"c55\x05power"
        invalid = [b"", valid + b"x", valid[:-1]]
        for offset, value in ((0, 0), (2, 0), (4, 0), (6, 0x8000)):
            payload = bytearray(valid)
            struct.pack_into("<H", payload, offset, value)
            invalid.append(bytes(payload))
        invalid.extend([
            struct.pack("<HHHHB", 1, 1, 2, 0, 3) + b"c55\x05power\x05power",
            valid.replace(b"power", b"pow_r"),
            valid.replace(b"c55", b"c\xff5"),
        ])
        self.assertEqual(decode_hello(valid), (1, 1, "c55", ("power",)))
        self.assertEqual(benchmark.decode_ui_hello(valid), {
            "width": 1, "height": 1, "model": "c55", "keys": ["power"],
            "capabilities": 3})
        for payload in invalid:
            with self.subTest(payload=payload):
                with self.assertRaises(protocol.ProtocolError):
                    protocol.decode_hello(payload)
                with self.assertRaises(CaptureError):
                    decode_hello(payload)
                with self.assertRaises(benchmark.BenchmarkError):
                    benchmark.decode_ui_hello(payload)

    def test_qemu_v7_hello_keeps_explicit_legacy_contract(self):
        payload = struct.pack("<HHHHB", 0, 1, 1, 0, 3) + b"c55\x05power"
        self.assertEqual(benchmark.decode_ui_hello(payload, version=7)["width"], 0)
        with self.assertRaises(benchmark.BenchmarkError):
            benchmark.decode_ui_hello(payload)

    def test_ui_hello_catalog_parser(self):
        keys = (b"soft-left", b"power")
        payload = bytearray(struct.pack("<HHHHB", 101, 64, 2, 0, 3) + b"c55")
        for key in keys:
            payload.append(len(key))
            payload.extend(key)
        self.assertEqual(
            benchmark.decode_ui_hello(bytes(payload))["keys"],
            ["soft-left", "power"],
        )
        with self.assertRaises(benchmark.BenchmarkError):
            benchmark.decode_ui_hello(bytes(payload) + b"x")

    def test_median_aggregation(self):
        medians = benchmark.aggregate_samples([
            rate_sample(30.0), rate_sample(10.0), rate_sample(20.0),
        ])
        self.assertEqual(medians, rate_sample(20.0))

    def test_baseline_comparison(self):
        current = {"scenarios": [{"name": "active-reset", "median": rate_sample(120.0)}]}
        baseline = {"scenarios": [{"name": "active-reset", "median": rate_sample(100.0)}]}
        compared = benchmark.compare_reports(current, baseline)
        self.assertEqual(compared[0]["name"], "active-reset")
        for value in compared[0]["median_delta_percent"].values():
            self.assertAlmostEqual(value, 20.0)

    def test_cross_engine_ratios_are_explicitly_qemu_over_cemu(self):
        engines = {}
        for engine, value in (("cemu", 2.0), ("qemu", 3.0)):
            engines[engine] = {"scenarios": [{
                "name": "ui-boot",
                "median": {key: value for key in benchmark.MILESTONE_MEDIAN_KEYS},
            }]}
        ratios = benchmark.cross_engine_ratios(engines)
        self.assertEqual(ratios[0]["name"], "ui-boot")
        for value in ratios[0]["qemu_over_cemu"].values():
            self.assertEqual(value, 1.5)

    def test_ui_packet_decoder_handles_partial_packets(self):
        packet = benchmark.encode_ui_packet(
            benchmark.UI_STATS, benchmark.UI_STATS_PAYLOAD.pack(10, 20, 15, 0x1234, 1, 100, 0, 0.0, 0.0, 0), sequence=7, icount=1234
        )
        decoded, remaining = benchmark.decode_ui_packets(packet[:10])
        self.assertEqual(decoded, [])
        self.assertEqual(remaining, packet[:10])
        decoded, remaining = benchmark.decode_ui_packets(remaining + packet[10:])
        self.assertEqual(decoded, [(benchmark.UI_STATS, benchmark.UI_STATS_PAYLOAD.pack(10, 20, 15, 0x1234, 1, 100, 0, 0.0, 0.0, 0), 7, 1234)])
        self.assertEqual(remaining, b"")


class ProcessTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="cemu-benchmark-test-")
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def test_failed_child_process_is_reported(self):
        executable = self.root / "fail-cemu"
        write_executable(executable, "#!/bin/sh\necho deliberate failure >&2\nexit 7\n")
        with self.assertRaises(benchmark.BenchmarkError) as caught:
            benchmark.run_cemu([str(executable)])
        self.assertIn("exited 7", str(caught.exception))
        self.assertIn("deliberate failure", str(caught.exception))

    def test_post_boot_prepares_snapshot_then_resumes(self):
        executable = self.root / "fake-cemu"
        log_path = self.root / "calls.jsonl"
        make_fake_cemu(executable, log_path)
        results = benchmark.run_suite(
            str(executable), str(executable), str(self.root / "flash.bin"), ["post-boot"],
            runs=1, warmup=0, quiet=True,
            cemu_args=["--batch-idle"],
        )
        calls = [json.loads(line) for line in log_path.read_text().splitlines()]
        self.assertEqual(len(calls), 2)
        self.assertIn("--snapshot", calls[0])
        self.assertIn("--batch-idle", calls[0])
        self.assertIn("20m", calls[0])
        self.assertIn("--from-snapshot", calls[1])
        self.assertIn("--batch-idle", calls[1])
        self.assertIn("50m", calls[1])
        sample = results[0]["samples"][0]
        self.assertEqual(sample["start_icount"], 20000000)
        self.assertEqual(sample["end_icount"], 70000000)
        self.assertEqual(sample["ticks"], 50000000)

    def test_connected_ui_measures_from_first_stats(self):
        executable = self.root / "fake-ui-cemu"
        make_fake_ui_cemu(executable)
        sample = benchmark.run_connected_ui(
            str(executable), str(self.root / "flash.bin"), ["--limit", "1k"], self.root,
        )
        self.assertEqual(sample["status"], "limit")
        self.assertEqual(sample["start_icount"], 100)
        self.assertEqual(sample["end_icount"], 1000)
        self.assertEqual(sample["ticks"], 900)
        self.assertEqual(
            sample["ui_messages"], {"hello": 1, "stats": 1, "frame": 1}
        )
        self.assertIsNone(sample["emulator_guest_instructions_per_s"])

    def test_ui_limit_is_used_for_warmup_and_measured_runs(self):
        executable = self.root / "fake-ui-cemu"
        make_fake_ui_cemu(executable)
        results = benchmark.run_suite(str(executable), str(executable), "flash.bin",
                                      ["ui-connected"], runs=1, warmup=1,
                                      ui_limit=50000000, quiet=True)
        self.assertEqual(results[0]["arguments"], ["--limit", "50000000"])
        for sample in results[0]["samples"] + results[0]["warmup_samples"]:
            self.assertEqual(sample["end_icount"], 50000000)

    def test_optional_affinity_is_restored(self):
        with mock.patch.object(
                benchmark.os, "sched_getaffinity", return_value={2, 3},
                create=True), mock.patch.object(
                benchmark.os, "sched_setaffinity", create=True) as setter:
            with benchmark.pinned_cpu(2) as affinity:
                self.assertEqual(affinity, [2])
            self.assertEqual(
                setter.call_args_list,
                [mock.call(0, {2}), mock.call(0, {2, 3})],
            )

    def test_softkey_milestone_uses_protocol_v13_and_segment_deltas(self):
        self.check_softkey_milestone("cemu", 13)

    def test_qemu_softkey_milestone_uses_host_protocol_v13_and_segment_deltas(self):
        self.check_softkey_milestone("qemu", 13)

    def check_softkey_milestone(self, engine, version):
        executable = self.root / "fake-milestone"
        log_path = self.root / "milestone.json"
        make_fake_milestone_engine(executable, log_path, version)
        boot_digest = hashlib.sha256(b"boot").hexdigest()
        dialog_digest = hashlib.sha256(b"dialog").hexdigest()
        old_boot = benchmark.UI_BOOT_DIGEST
        old_target = benchmark.SCENARIOS["ui-softkey"]["target_digest"]
        benchmark.UI_BOOT_DIGEST = boot_digest
        benchmark.SCENARIOS["ui-softkey"]["target_digest"] = dialog_digest
        try:
            sample = benchmark.run_ui_milestone(
                engine, str(executable), str(self.root / "flash.bin"),
                "ui-softkey", self.root, extra_args=("-d", "exec"),
                timeout_s=5, qemu_supervisor=str(executable),
            )
        finally:
            benchmark.UI_BOOT_DIGEST = old_boot
            benchmark.SCENARIOS["ui-softkey"]["target_digest"] = old_target
        logged = json.loads(log_path.read_text())
        self.assertEqual(logged["types"], [benchmark.UI_KEY,
                                           benchmark.UI_KEY_RELEASE_AFTER_SAMPLE])
        self.assertEqual(logged["args"][-2:], ["-d", "exec"])
        self.assertEqual(sample["ticks"], 1000)
        self.assertEqual(sample["guest_instructions"], 800)
        self.assertEqual(sample["ui_messages"], {"hello": 0, "stats": 1,
                                                  "frame": 1})
        self.assertEqual(sample["final_frame_digest"], dialog_digest)

    def test_qemu_throughput_window_uses_tick_and_guest_deltas(self):
        executable = self.root / "fake-throughput"
        make_fake_throughput_engine(executable)
        sample = benchmark.run_qemu_throughput(
            str(executable), str(self.root / "flash.bin"),
            "qemu-no-sim-throughput", self.root,
            duration_s=0.03, warmup_ticks=4_000_000,
            qemu_supervisor=str(executable),
        )
        self.assertEqual(sample["status"], "window")
        self.assertGreaterEqual(sample["ticks"], 2_000_000)
        self.assertEqual(sample["guest_instructions"], sample["ticks"] // 2)

    def test_bundled_throughput_has_no_inherited_qualification(self):
        self.assertIsNone(benchmark.QEMU_QUALIFICATION_BASELINES)
        gate = benchmark.evaluate_qemu_throughput([])
        self.assertEqual(gate["status"], "not-qualified")
        self.assertIsNone(gate["passed"])
        self.assertEqual(gate["checks"], [])

    def test_qemu_throughput_gate_checks_absolute_and_baselines(self):
        scenarios = []
        for name, value in (
                ("qemu-no-sim-throughput", 20_000_000.0),
                ("qemu-sim-throughput", 18_000_000.0)):
            scenarios.append({
                "name": name,
                "median": {"process_wall_ticks_per_s": value},
            })
        gate = benchmark.evaluate_qemu_throughput(
            scenarios,
            minimum_no_sim=20_000_000.0,
            baselines={
                "qemu-no-sim-throughput": 20_000_000.0,
                "qemu-sim-throughput": 20_000_000.0,
            },
        )
        self.assertTrue(gate["passed"])
        scenarios[1]["median"]["process_wall_ticks_per_s"] = 17_999_999.0
        self.assertFalse(benchmark.evaluate_qemu_throughput(
            scenarios,
            minimum_no_sim=20_000_000.0,
            baselines={
                "qemu-no-sim-throughput": 20_000_000.0,
                "qemu-sim-throughput": 20_000_000.0,
            },
        )["passed"])


if __name__ == "__main__":
    unittest.main()
