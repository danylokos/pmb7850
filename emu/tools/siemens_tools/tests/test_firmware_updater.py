from __future__ import annotations

import contextlib
import hashlib
import io
import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from tools.siemens_tools.firmware.cli import main as firmware_main
from tools.siemens_tools.firmware.updater import (
    IMAGE_FILENAME,
    MANIFEST_FILENAME,
    TRANSPORT_FILENAME,
    parse_mobile_updater,
    parse_updater_frames,
)
from tools.siemens_tools.firmware.updater_analysis import (
    UpdaterSemanticProfile,
    describe_firmware_payloads,
    find_flash_configuration_records,
    find_relocated_updater_matches,
    find_statistics_finalization_constructors,
    match_semantic_profile,
)
from tools.siemens_tools.firmware.xbi import FirmwareError, SAG_JK_WH
from tools.siemens_tools.tests.firmware_fixtures import build_xbi


def _xor(data: bytes) -> int:
    value = 0
    for byte in data:
        value ^= byte
    return value


def _frame(base: int, offset: int, payload: bytes) -> bytes:
    prefix = (
        base.to_bytes(2, "little")
        + bytes((offset, len(payload)))
        + payload
    )
    return prefix + bytes((_xor(prefix),))


def _section(
    name: bytes, virtual_address: int, raw_offset: int, raw_size: int
) -> bytes:
    entry = bytearray(40)
    entry[:len(name)] = name
    entry[8:12] = raw_size.to_bytes(4, "little")
    entry[12:16] = virtual_address.to_bytes(4, "little")
    entry[16:20] = raw_size.to_bytes(4, "little")
    entry[20:24] = raw_offset.to_bytes(4, "little")
    return bytes(entry)


def _service_exe(stream: bytes) -> bytes:
    image = bytearray(0x900)
    image[:2] = b"MZ"
    image[0x3C:0x40] = (0x80).to_bytes(4, "little")
    image[0x80:0x84] = b"PE\0\0"
    image[0x84:0x86] = (0x14C).to_bytes(2, "little")
    image[0x86:0x88] = (3).to_bytes(2, "little")
    image[0x94:0x96] = (0xE0).to_bytes(2, "little")
    optional = 0x98
    image[optional:optional + 2] = (0x10B).to_bytes(2, "little")
    image[optional + 28:optional + 32] = (0x400000).to_bytes(4, "little")
    sections = optional + 0xE0
    image[sections:sections + 40] = _section(b".text", 0x1000, 0x400, 0x200)
    image[sections + 40:sections + 80] = _section(
        b".rdata", 0x2000, 0x600, 0x200
    )
    image[sections + 80:sections + 120] = _section(
        b".data", 0x3000, 0x800, 0x100
    )
    accessor = (
        bytes.fromhex("8B 44 24 04 8B 0D")
        + (0x403020).to_bytes(4, "little")
        + bytes.fromhex("89 08 B8")
        + (0x402020).to_bytes(4, "little")
        + b"\xC3"
    )
    image[0x420:0x420 + len(accessor)] = accessor
    image[0x620:0x620 + len(stream)] = stream
    image[0x820:0x824] = len(stream).to_bytes(4, "little")
    return bytes(image) + SAG_JK_WH


class MobileUpdaterTests(unittest.TestCase):
    def setUp(self) -> None:
        self.stream = b"".join((
            _frame(0x0200, 0, bytes.fromhex("FA 00 5C 30")),
            _frame(0x0200, 0x10, b"fixture"),
        ))

    def test_pe_accessor_frames_and_address_preserving_image(self) -> None:
        extraction = parse_mobile_updater(_service_exe(self.stream))

        self.assertEqual(extraction.source_offset, 0x620)
        self.assertEqual(extraction.stream, self.stream)
        self.assertEqual(len(extraction.frames), 2)
        self.assertEqual(extraction.frames[1].address, 0x210)
        self.assertEqual(extraction.image[0x200:0x204], bytes.fromhex("FA005C30"))
        self.assertEqual(extraction.image[0x210:0x217], b"fixture")
        self.assertEqual(extraction.image[0x204:0x210], b"\xFF" * 12)
        self.assertEqual(extraction.entry_transfer["source"], 0x0200)
        self.assertEqual(extraction.entry_transfer["target"], 0x305C)

    def test_frame_checksum_and_overlap_fail_closed(self) -> None:
        damaged = bytearray(self.stream)
        damaged[4] ^= 1
        with self.assertRaisesRegex(FirmwareError, "checksum"):
            parse_updater_frames(bytes(damaged))

        overlap = _frame(0x0200, 0, b"AB") + _frame(0x0200, 1, b"BC")
        with self.assertRaisesRegex(FirmwareError, "overlapping"):
            parse_updater_frames(overlap)

    def test_info_json_and_extract_force_preserve_unrelated_file(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "fixture_service.exe"
            output = root / "updater"
            source.write_bytes(_service_exe(self.stream))
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                status = firmware_main(["updater", "info", str(source), "--json"])
            description = json.loads(stdout.getvalue())

            with contextlib.redirect_stdout(io.StringIO()):
                extract_status = firmware_main([
                    "updater", "extract", str(source), "-o", str(output)
                ])
            unrelated = output / "keep.txt"
            unrelated.write_text("keep", encoding="ascii")
            with contextlib.redirect_stderr(io.StringIO()):
                existing_status = firmware_main([
                    "updater", "extract", str(source), "-o", str(output)
                ])
            with contextlib.redirect_stdout(io.StringIO()):
                force_status = firmware_main([
                    "updater", "extract", str(source), "-o", str(output),
                    "--force",
                ])
            manifest = json.loads((output / MANIFEST_FILENAME).read_text())
            unrelated_text = unrelated.read_text(encoding="ascii")
            transport = (output / TRANSPORT_FILENAME).read_bytes()
            image_digest = hashlib.sha256(
                (output / IMAGE_FILENAME).read_bytes()
            ).hexdigest()

        self.assertEqual(status, 0)
        self.assertEqual(description["transport"]["frame_count"], 2)
        self.assertEqual(extract_status, 0)
        self.assertEqual(existing_status, 1)
        self.assertEqual(force_status, 0)
        self.assertEqual(unrelated_text, "keep")
        self.assertEqual(manifest["transport"]["frames"][0]["source_range"]["start"], 0x620)
        self.assertEqual(transport, self.stream)
        self.assertEqual(image_digest, manifest["image"]["sha256"])

    def test_schema_v3_replaces_trampoline_claims(self) -> None:
        data = _service_exe(self.stream)
        extraction = parse_mobile_updater(data)
        from tools.siemens_tools.firmware.updater import describe_mobile_updater

        description = describe_mobile_updater(
            Path("fixture_service.exe"), data, extraction
        )
        self.assertEqual(description["schema_version"], 3)
        for key in ("input", "locator", "transport", "image"):
            self.assertIn(key, description)
        self.assertIn("firmware_payloads", description)
        self.assertIn("generation", description)
        self.assertIn("semantics", description)
        self.assertIn("firmware_comparison", description)

        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "fixture_service.exe"
            source.write_bytes(data)
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                status = firmware_main(["updater", "info", str(source)])
        self.assertEqual(status, 0)
        self.assertIn("updater_profile: unrecognized", stdout.getvalue())
        self.assertIn("finalization_write_status: unrecognized", stdout.getvalue())
        self.assertIn("statistics_status: unrecognized", stdout.getvalue())
        self.assertIn("relocated_image_status: not-run", stdout.getvalue())
        self.assertNotIn("trampoline", stdout.getvalue())

    def test_exact_profile_matching_and_model_conflicts(self) -> None:
        stream = b"profile stream"
        image = b"profile image"
        profile = UpdaterSemanticProfile(
            "fixture-profile", frozenset(("T55",)),
            hashlib.sha256(stream).hexdigest(),
            hashlib.sha256(image).hexdigest(),
            0x100, 0x110, 0x200, 0x300, 0x400,
            0x500, 0x600, (0x700, 0x702), 0x800,
        )
        extraction = SimpleNamespace(stream=stream, image=image)
        with patch(
            "tools.siemens_tools.firmware.updater_analysis.UPDATER_PROFILES",
            (profile,),
        ):
            matched, reason = match_semantic_profile(
                extraction, [{"status": "confirmed", "model": "T55"}]
            )
            conflict, conflict_reason = match_semantic_profile(
                extraction, [{"status": "confirmed", "model": "X55"}]
            )
            mixed, mixed_reason = match_semantic_profile(
                extraction, [
                    {"status": "confirmed", "model": "T55"},
                    {"status": "confirmed", "model": "X55"},
                ],
            )
            unknown, unknown_reason = match_semantic_profile(
                SimpleNamespace(stream=stream + b"!", image=image),
                [{"status": "confirmed", "model": "T55"}],
            )
        self.assertIs(matched, profile)
        self.assertIsNone(reason)
        self.assertIsNone(conflict)
        self.assertIn("model conflict", conflict_reason)
        self.assertIsNone(mixed)
        self.assertIn("model conflict", mixed_reason)
        self.assertIsNone(unknown)
        self.assertIn("no exact", unknown_reason)

    def test_finalization_constructor_zero_one_and_multiple(self) -> None:
        signature = bytes.fromhex(
            "E6 FC FA 07 B8 C0 E6 FD F0 FF C4 D0 02 00"
        )
        one = b"\0" * 0x16 + signature
        self.assertEqual(
            find_statistics_finalization_constructors(b"no signature"), []
        )
        self.assertEqual(find_statistics_finalization_constructors(one), [0])
        self.assertEqual(
            find_statistics_finalization_constructors(
                one + b"\0" * 0x16 + signature
            ),
            [0, len(one)],
        )

    def test_flash_configuration_record_decodes_distinct_addresses(self) -> None:
        record = bytes.fromhex(
            "E6 F1 FC FF E0 72 88 20 88 10 "
            "E6 F3 00 FE E6 F4 87 00 88 40 88 30 "
            "DA 00 00 01"
        )
        parsed = find_flash_configuration_records(record, 0x0100)

        self.assertEqual(len(parsed), 1)
        self.assertEqual(parsed[0].site_address, 0)
        self.assertEqual(parsed[0].initializer_callsite_address, 0x16)
        self.assertEqual(parsed[0].statistic_address, 0x87FE00)
        self.assertEqual(parsed[0].finalization_address, 0x07FFFC)

    def test_ancillary_payload_parse_failure_is_isolated(self) -> None:
        valid = build_xbi(writes=[(0x20, b"payload")])
        malformed = valid[:60]
        with patch(
            "tools.siemens_tools.firmware.updater_analysis.extract_exe",
            return_value=[valid, malformed, b"ancillary"],
        ):
            payloads, parsed = describe_firmware_payloads(
                b"service", b"updater stream"
            )
        self.assertEqual(payloads[0]["status"], "confirmed")
        self.assertEqual(payloads[1]["status"], "unresolved")
        self.assertIn("error", payloads[1])
        self.assertEqual(payloads[2]["status"], "unrecognized")
        self.assertEqual([item[0] for item in parsed], [0])

    def test_relocated_image_requires_exact_owned_mapped_bytes(self) -> None:
        extraction = parse_mobile_updater(_service_exe(self.stream))

        def relocated(delta: int) -> tuple[bytes, bytes]:
            size = len(extraction.image) + delta + 0x100
            flash = bytearray(b"\xFF" * size)
            mask = bytearray(size)
            for item in extraction.mapped_ranges:
                start = item.start + delta
                end = item.end + delta
                flash[start:end] = extraction.image[item.start:item.end]
                mask[start:end] = b"\x01" * (end - start)
            return bytes(flash), bytes(mask)

        flash, mask = relocated(0x100)
        self.assertEqual(
            find_relocated_updater_matches(extraction, flash, mask), [0x100]
        )
        shifted_flash, shifted_mask = relocated(0x240)
        self.assertEqual(
            find_relocated_updater_matches(
                extraction, shifted_flash, shifted_mask
            ),
            [0x240],
        )
        damaged = bytearray(flash)
        damaged[extraction.mapped_ranges[0].start + 0x100] ^= 1
        self.assertEqual(
            find_relocated_updater_matches(extraction, bytes(damaged), mask), []
        )
        not_owned = bytearray(mask)
        not_owned[extraction.mapped_ranges[-1].end - 1 + 0x100] = 0
        self.assertEqual(
            find_relocated_updater_matches(
                extraction, flash, bytes(not_owned)
            ),
            [],
        )





if __name__ == "__main__":
    unittest.main()
