from __future__ import annotations

import asyncio
import contextlib
import hashlib
import json
import struct
import tempfile
import unittest
from pathlib import Path

from tools.info_menu_capture import CaptureError, HELLO_PREFIX, KEY, KEY_RELEASE_AFTER_SAMPLE, PAGE_NAMES, UiCapture, command_for, decode_hello, firmware_metadata, inventory_diff, pages_through, refresh_screen_data, replay_signature, save_frame, scroll_fixed, write_json


class InfoMenuCaptureTests(unittest.TestCase):
    def test_firmware_metadata_manifest_fields(self) -> None:
        data = bytearray(b"\xff" * 0x80000)
        record = memoryview(data)[0x7ff50:0x7ffe0]
        record[0] = 0x24
        record[1:10] = bytes((0xff, 0x0a, 0x50, 0x14, 0x14,
                              0x01, 0x00, 0xd5, 0x21))
        record[0x10:0x15] = b"lg91\0"
        record[0x20:0x24] = b"C55\0"
        record[0x30:0x38] = b"SIEMENS\0"
        data[0x32c] = 0x07
        struct.pack_into("<HH", data, 0x7fe26, 0x0089, 0x0017)
        self.assertEqual(firmware_metadata(bytes(data)), [{
            "metadata_offset": 0x7ff50, "metadata_view_offset": 0,
            "model": "C55", "software_version": 24,
            "software_version_raw": 0x24, "langpack": "lg91",
            "bcore_software_version": 7, "bcore_software_version_raw": 0x07,
            "flash_manufacturer_id": 0x0089, "flash_device_id": 0x0017,
        }])

    def test_hello_key_order_and_command_identity(self) -> None:
        keys = ("star", "hash", "0", "6", "down", "soft-left")
        payload = HELLO_PREFIX.pack(101, 80, len(keys), 1) + bytes((3,)) + b"m55"
        payload += b"".join(bytes((len(key),)) + key.encode() for key in keys)
        self.assertEqual(decode_hello(payload), (101, 80, "m55", keys))
        entry = {"source_path": "fw/m55.bin", "device": "m55",
                 "fsn": "C8AAE55F", "imei": "35202600729559",
                 "identity_mode": "native-fsn"}
        command = command_for(entry, Path("/repo/emu/shots/job/image/run-1"),
                              Path("/tmp/ui.sock"), 200_000_000, Path("/repo/emu"))
        self.assertIn("--fsn", command)
        self.assertIn("--imei", command)
        self.assertNotIn("--eeprom-overlay", command)
        self.assertNotIn("--sim", command)
        self.assertEqual(command[command.index("--trace=eeprom,keypad,lcd,lifecycle,serial")],
                         "--trace=eeprom,keypad,lcd,lifecycle,serial")
        entry["identity_mode"] = "built-in-eeprom"
        built_in = command_for(entry, Path("/repo/emu/shots/job/image/run-1"),
                               Path("/tmp/ui.sock"), 200_000_000,
                               Path("/repo/emu"))
        self.assertNotIn("--fsn", built_in)
        self.assertNotIn("--imei", built_in)

    def test_png_hash_is_deterministic(self) -> None:
        frame = bytes((x % 256 for x in range(3 * 2 * 3)))
        with tempfile.TemporaryDirectory() as tmp:
            first = save_frame(Path(tmp), "first", frame, 3, 2)
            second = save_frame(Path(tmp), "second", frame, 3, 2)
            self.assertEqual(first["raw_rgb_sha256"], hashlib.sha256(frame).hexdigest())
            self.assertEqual(first["native_png_sha256"], second["native_png_sha256"])
            png = (Path(tmp) / "first-native.png").read_bytes()
            self.assertEqual(struct.unpack_from(">II", png, 16), (3, 2))

    def test_manifest_json_and_scroll_stop_are_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            first, second = Path(tmp) / "one.json", Path(tmp) / "two.json"
            write_json(first, {"z": 1, "a": 2})
            write_json(second, {"a": 2, "z": 1})
            self.assertEqual(first.read_bytes(), second.read_bytes())
        frame = b"\x01\x02\x03"
        self.assertTrue(scroll_fixed(hashlib.sha256(frame).hexdigest(), frame))
        self.assertFalse(scroll_fixed("0" * 64, frame))

    def test_status_only_capture_terminates_before_cc_monitor(self) -> None:
        self.assertEqual(pages_through("status"), ("imei", "status"))
        self.assertNotIn("cc-monitor", pages_through("status"))

    def test_inventory_diff_and_replay_signature(self) -> None:
        before = {"blocks": [{"id": 1, "length": 2, "payload_sha256": "a"},
                             {"id": 2, "length": 2, "payload_sha256": "b"}]}
        after = {"blocks": [{"id": 1, "length": 2, "payload_sha256": "a"},
                            {"id": 3, "length": 1, "payload_sha256": "c"}]}
        self.assertEqual(inventory_diff(before, after), {
            "added": [3], "removed": [2], "changed": [], "unchanged": [1]})
        result = {"captures": [
            {"action_index": 4, "page": PAGE_NAMES[0], "raw_rgb_sha256": "x"},
            {"action_index": 6, "page": PAGE_NAMES[1], "raw_rgb_sha256": "y"},
        ]}
        self.assertEqual(replay_signature(result), [(4, "imei", "x"),
                                                    (6, "status", "y")])

    def test_screen_data_refresh_preserves_manual_transcription(self) -> None:
        manifest = {"images": [{
            "slug": "c55-one", "source_sha256": "a" * 64,
            "identity_mode": "built-in-eeprom",
        }]}
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp)
            run = output / "c55-one/run-1"
            run.mkdir(parents=True)
            (output / "c55-one/result.json").write_text(json.dumps({
                "classification": "pass", "run": "run-1"}), encoding="utf-8")
            (run / "capture.json").write_text(json.dumps({"captures": [{
                "page": "imei", "position": 1, "raw_rgb_sha256": "b" * 64,
            }]}), encoding="utf-8")
            refresh_screen_data(output, manifest)
            screen = json.loads((output / "screen-data.json").read_text())
            screen["images"][0]["transcription"] = {"imei": "123"}
            (output / "screen-data.json").write_text(json.dumps(screen))
            refresh_screen_data(output, manifest)
            refreshed = json.loads((output / "screen-data.json").read_text())
            self.assertEqual(refreshed["images"][0]["transcription"],
                             {"imei": "123"})
            self.assertEqual(refreshed["images"][0]["frame_hashes"]["imei"][0]
                             ["raw_rgb_sha256"], "b" * 64)


class _Writer:
    def __init__(self) -> None:
        self.packets: list[bytes] = []

    def write(self, packet: bytes) -> None:
        self.packets.append(packet)

    async def drain(self) -> None:
        return None


class InfoMenuCaptureAsyncTests(unittest.IsolatedAsyncioTestCase):
    async def test_protocol_failures_become_capture_errors(self):
        from tools import ui_protocol as protocol
        hello = HELLO_PREFIX.pack(1, 1, 1, 0) + b"\x03c55\x05power"
        valid = protocol.encode_packet(protocol.HELLO, hello)
        malformed_header = bytearray(valid)
        malformed_header[0] ^= 1
        bad_stats = protocol.STATS_PAYLOAD.pack(0, 0, 0, 0, 1, 1, 0, 1.0, 0.0, 0)
        cases = {
            "framing": bytes(malformed_header),
            "hello": protocol.encode_packet(protocol.HELLO, b""),
            "serial": valid + protocol.encode_packet(protocol.ASC0_SUBSCRIBED, b""),
            "stats": valid + protocol.encode_packet(protocol.STATS, bad_stats),
            "lifecycle": valid + protocol.encode_packet(protocol.LIFECYCLE, b"\xff\x00\x00\x00"),
        }
        for name, wire in cases.items():
            with self.subTest(name=name):
                reader = asyncio.StreamReader()
                reader.feed_data(wire)
                reader.feed_eof()
                capture = UiCapture(reader, _Writer())
                if name in ("stats", "lifecycle", "serial"):
                    await capture.receive()
                with self.assertRaises(CaptureError) as caught:
                    await capture.receive()
                self.assertIsInstance(caught.exception.__cause__, protocol.ProtocolError)

    async def test_native_v13_capture_and_controls(self):
        from tools import ui_protocol
        from tools.info_menu_capture import connect_socket
        from tools.tests.shared.test_benchmark import DriverIntegrationTests
        cemu, flash = DriverIntegrationTests.cemu, DriverIntegrationTests.flash
        self.assertTrue(cemu.is_file(), "build CEMU before running host tests")
        with tempfile.TemporaryDirectory(prefix="info-native-v13-") as directory:
            path = Path(directory) / "ui.sock"
            process = await asyncio.create_subprocess_exec(str(cemu), str(flash),
                "--limit", "1m", "--ui-socket", str(path), "--benchmark-json",
                stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
            writer = None
            try:
                reader, writer = await connect_socket(path, process)
                ui = UiCapture(reader, writer)
                self.assertEqual(await ui.receive(), ui_protocol.HELLO)
                self.assertEqual(await ui.receive(), ui_protocol.LIFECYCLE)
                self.assertEqual(await ui.receive(), ui_protocol.STATS)
                self.assertEqual(ui.lifecycle["phase"], "running")
                await ui.send(ui_protocol.RELEASE_ALL)
                while ui.lifecycle["phase"] != "stopped" or ui.state.ticks < 1000000:
                    await ui.receive()
                self.assertEqual(ui.lifecycle["status"], "limit")
                self.assertNotEqual(ui.run_id, bytes(16))
                stdout, stderr = await process.communicate()
                self.assertEqual(process.returncode, 0, stderr.decode())
                self.assertEqual(json.loads(stdout)["end_icount"], ui.state.ticks)
            finally:
                if writer:
                    writer.close()
                    with contextlib.suppress(Exception):
                        await writer.wait_closed()
                if process.returncode is None:
                    process.kill()
                    await process.wait()

    async def test_tap_sends_down_then_sampled_release(self) -> None:
        writer = _Writer()
        ui = UiCapture(None, writer)  # type: ignore[arg-type]
        ui.state.keys = ("star", "hash")
        ui.state.icount = 100
        ui.state.frame = b"\x00\x00\x00"

        async def advance(start: int, delta: int, *, timeout: float = 30.0) -> None:
            del timeout
            ui.state.icount = start + delta

        ui.wait_ticks = advance  # type: ignore[method-assign]
        action = await ui.tap("hash", settle_ticks=25)
        kinds = [struct.unpack_from("<H", packet, 6)[0] for packet in writer.packets]
        self.assertEqual(kinds, [KEY, KEY_RELEASE_AFTER_SAMPLE])
        self.assertEqual([packet[48:] for packet in writer.packets],
                         [b"\x01\x01", b"\x01"])
        self.assertEqual(action["release"], "after-sample")
        self.assertEqual(action["settled_icount"], 125)


if __name__ == "__main__":
    unittest.main()
