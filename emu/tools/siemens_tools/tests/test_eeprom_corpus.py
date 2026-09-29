#!/usr/bin/env python3
"""EEPROM corpus discovery, clustering, and CLI tests."""

from __future__ import annotations

import json
import tempfile
import unittest
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path

from tools.siemens_tools.eeprom import (  # noqa: E402
    A52_EMULATOR_MINIMAL_PROFILE,
    A55_EMULATOR_MINIMAL_PROFILE,
    A60_EMULATOR_MINIMAL_PROFILE,
    A62_EMULATOR_MINIMAL_PROFILE,
    A65_EMULATOR_MINIMAL_PROFILE,
    CORPUS_SCHEMA,
    C55_EMULATOR_MINIMAL_PROFILE,
    C60_EMULATOR_MINIMAL_PROFILE,
    CF62_EMULATOR_MINIMAL_PROFILE,
    M55_EMULATOR_MINIMAL_PROFILE,
    MC60_EMULATOR_MINIMAL_PROFILE,
    SL55_EMULATOR_MINIMAL_PROFILE,
    S55_EMULATOR_MINIMAL_PROFILE,
    analyze_corpus,
    discover_sources,
    get_eeprom_profile,
    render_corpus,
)
from tools.siemens_tools.eeprom.cli import main  # noqa: E402


def map_text(blocks: dict[int, bytes]) -> str:
    sections = ["[MapFileInfo]\nProduct = 130\nSWVersion = 24\n"]
    for block_id, payload in blocks.items():
        rendered = " ".join(f"0x{value:02X}" for value in payload)
        sections.append(
            f"[{block_id}] ; synthetic {block_id}\n"
            "Offset = 0\nMemory = 2\nVersion = 1\n"
            f"DataSize = {len(payload)}\nData {{\n{rendered}\n}}\n"
        )
    sections.append("[CheckSum]\nKey = 0\n")
    return "\n".join(sections)


class SyntheticCorpusTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        nested = self.root / "nested"
        nested.mkdir()
        (self.root / "a.map").write_text(
            map_text({1: b"\x10\x20", 2: b"\x01\x02"}), encoding="ascii"
        )
        (nested / "b.map").write_text(
            map_text({1: b"\x10\x20", 2: b"\x01\x03"}), encoding="ascii"
        )
        (nested / "copy.map").write_text(
            map_text({1: b"\x10\x20", 2: b"\x01\x02"}), encoding="ascii"
        )
        (nested / "broken.bin").write_bytes(b"not an EEPROM dump")
        (nested / "ignored.txt").write_text("ignored", encoding="ascii")
        split_output = self.root / "capture.split"
        split_output.mkdir()
        (split_output / "region.bin").write_bytes(b"generated")

    def test_discovery_is_recursive_deduplicated_and_sorted(self) -> None:
        paths = discover_sources([self.root, self.root / "nested"])
        self.assertEqual(len(paths), 4)
        self.assertEqual(paths, sorted(paths, key=lambda path: path.as_posix()))
        self.assertTrue(all(path.suffix in {".bin", ".map"} for path in paths))
        self.assertEqual(
            discover_sources([self.root / "capture.split"]),
            [self.root / "capture.split/region.bin"],
        )

    def test_clusters_variants_profiles_and_consensus(self) -> None:
        report = analyze_corpus([self.root], [1, 2, 1])
        self.assertEqual(report["schema"], CORPUS_SCHEMA)
        self.assertEqual(report["requested_blocks"], [1, 2])
        self.assertEqual(report["summary"], {
            "candidates": 4,
            "parsed": 3,
            "parse_failed": 1,
            "target_bearing": 3,
            "complete_occurrences": 3,
            "unique_source_files": 3,
            "profiles": 2,
            "complete_profiles": 2,
        })

        block1, block2 = report["blocks"]
        self.assertEqual(block1["unique_payloads"], 1)
        self.assertEqual(block1["variants"][0]["u16_le"], [0x2010])
        self.assertEqual(block1["variants"][0]["source_occurrences"], 3)
        self.assertEqual(block1["variants"][0]["unique_source_files"], 2)
        self.assertEqual(block2["unique_payloads"], 2)
        self.assertEqual(block2["consensus"]["varying_offsets"], [1])
        self.assertEqual(
            block2["consensus"]["invariant_spans"],
            [{"start": 0, "end": 1, "hex": "01"}],
        )
        self.assertEqual(
            sorted(profile["source_occurrences"] for profile in report["profiles"]),
            [1, 2],
        )

    def test_report_is_deterministic_and_keeps_parse_errors(self) -> None:
        first = analyze_corpus([self.root], [1, 2])
        second = analyze_corpus([self.root], [1, 2])
        self.assertEqual(first, second)
        failure = [
            source for source in first["sources"] if source["status"] == "error"
        ]
        self.assertEqual(len(failure), 1)
        self.assertIn("could not locate EEPROM region", failure[0]["error"])
        human = render_corpus(first)
        self.assertIn("4 candidates, 3 parsed, 1 failed", human)
        self.assertIn("parse failures:", human)

    def test_json_cli_and_all_failed_exit_status(self) -> None:
        output = StringIO()
        with redirect_stdout(output):
            result = main([
                "corpus", str(self.root), "--block", "1", "--block", "2", "--json",
            ])
        self.assertEqual(result, 0)
        self.assertEqual(json.loads(output.getvalue())["summary"]["profiles"], 2)

        failed_root = self.root / "failed"
        failed_root.mkdir()
        (failed_root / "bad.bin").write_bytes(b"bad")
        output = StringIO()
        with redirect_stdout(output):
            result = main(["corpus", str(failed_root), "--block", "1"])
        self.assertEqual(result, 1)
        self.assertIn("0 parsed", output.getvalue())

    def test_rejects_bad_inputs_and_block_ids(self) -> None:
        with self.assertRaisesRegex(ValueError, "does not exist"):
            discover_sources([self.root / "missing"])
        with self.assertRaisesRegex(ValueError, "range"):
            analyze_corpus([self.root], [0x10000])

    def test_named_profile_lookup_and_payload_geometry(self) -> None:
        common_donors = (5005, 5006)
        supported_versions = {
            "c55": (24, 85), "m55": (10, 11, 91), "a55": (9, 11), "a52": (9,),
            "mc60": (10, 13), "cf62": (7, 24, 95), "a60": (27,),
            "a62": (6, 7), "a65": (15, 17, 62), "c60": (20, 27),
            "sl55": (7, 20), "s55": (20, 91),
        }
        qualified_versions = {
            "c55": (24,), "m55": (91,), "a55": (9,), "a52": (9,),
            "mc60": (13,), "cf62": (7,), "a60": (27,), "a62": (7,),
            "a65": (15,), "c60": (20,), "sl55": (7,), "s55": (91,),
        }
        cases = {
            "c55": (C55_EMULATOR_MINIMAL_PROFILE, 130,
                    (1, 2, 55, 75, 5121, 5122, 5123)),
            "m55": (M55_EMULATOR_MINIMAL_PROFILE, 86,
                    (1, 2, 55, 75, 167, 5002, 5121, 5122, 5123, 5352)),
            "a55": (A55_EMULATOR_MINIMAL_PROFILE, 196,
                    (1, 2, 55, 57, 75, 144, 5121, 5122, 5123, 5180, 5181,
                     5255, 5256, 5257, 5258, 5259, 5372)),
            "a52": (A52_EMULATOR_MINIMAL_PROFILE, 226,
                    (1, 2, 55, 57, 75, 144, 5121, 5122, 5123, 5180, 5181,
                     5255, 5256, 5257, 5258, 5259, 5372)),
            "mc60": (MC60_EMULATOR_MINIMAL_PROFILE, 132,
                     (1, 2, 55, 75, 167, 5057, 5121, 5122, 5123,
                      5244, 5245, 5246, 5247, 5248, 5352)),
            "cf62": (CF62_EMULATOR_MINIMAL_PROFILE, 228,
                     (1, 2, 55, 75, 167, 5121, 5122, 5123, 5223,
                      5244, 5245, 5246, 5247, 5248, 5352, 5385, 5395)),
            "a60": (A60_EMULATOR_MINIMAL_PROFILE, 39,
                    (1, 2, 55, 75, 167, 5057, 5121, 5122, 5123,
                     5165, 5351, 5352)),
            "a62": (A62_EMULATOR_MINIMAL_PROFILE, 231,
                    (1, 2, 55, 75, 167, 5057, 5121, 5122, 5123,
                     5165, 5351, 5352, 5436, 5437, 5439)),
            "a65": (A65_EMULATOR_MINIMAL_PROFILE, 230,
                    (1, 2, 55, 75, 167, 5057, 5121, 5122, 5123,
                     5165, 5351, 5352)),
            "c60": (C60_EMULATOR_MINIMAL_PROFILE, 40,
                    (1, 2, 55, 75, 167, 5047, 5048, 5049, 5050, 5051,
                     5052, 5053, 5054, 5055, 5056, 5057, 5121, 5122,
                     5123, 5165, 5351, 5352)),
            "sl55": (SL55_EMULATOR_MINIMAL_PROFILE, 36,
                     (1, 2, 55, 75, 167, 5002, 5121, 5122, 5123, 5352)),
            "s55": (S55_EMULATOR_MINIMAL_PROFILE, 84,
                    (1, 2, 55, 75, 167, 5002, 5121, 5122, 5123)),
        }
        for model, (expected, product, zero_ids) in cases.items():
            with self.subTest(model=model):
                profile = get_eeprom_profile(f"{model}-emulator-minimal-v1")
                self.assertIs(profile, expected)
                self.assertEqual(profile.product, product)
                self.assertEqual(profile.software_versions, supported_versions[model])
                self.assertEqual(
                    profile.ui_boot_qualified_software_versions,
                    qualified_versions[model],
                )
                self.assertEqual(profile.observed_payload_bytes, 0)
                self.assertEqual(profile.omitted_blocks, ())
                self.assertEqual(
                    tuple(record.block_id for record in profile.records),
                    tuple(sorted((*zero_ids, 67))),
                )
                self.assertTrue(all(
                    record.block_id == 67 or not any(record.payload)
                    for record in profile.records
                ))
                self.assertEqual(
                    profile.donor_blocks,
                    (306, 5005, 5006, 5436, 5437)
                    if model == "a65" else common_donors,
                )
                self.assertIsNotNone(profile.map_sha256)
        self.assertEqual(len(C55_EMULATOR_MINIMAL_PROFILE.records[2].payload), 14)
        self.assertEqual(len(M55_EMULATOR_MINIMAL_PROFILE.records[2].payload), 24)
        self.assertEqual(
            len(C55_EMULATOR_MINIMAL_PROFILE.records[-1].payload), 14,
        )
        self.assertEqual(
            len(M55_EMULATOR_MINIMAL_PROFILE.records[-2].payload), 12,
        )
        with self.assertRaisesRegex(ValueError, "unknown EEPROM profile"):
            get_eeprom_profile("missing")



class BundledC55CorpusTests(unittest.TestCase):
    def test_inventory_variants_and_calibration_are_reproducible(self) -> None:
        from tools.bundled_firmware import entries, ROOT
        paths = [ROOT / row["image"] for row in entries("c55")]
        targets = [1, 2, 55, 67, 75, 5006, 5121, 5122, 5123]
        report = analyze_corpus(paths, targets)
        self.assertEqual(report, analyze_corpus(paths, targets))
        summary = report["summary"]
        self.assertEqual(summary["candidates"], len(paths))
        self.assertEqual(summary["parsed"], len(paths))
        self.assertEqual(summary["parse_failed"], 0)
        self.assertEqual(summary["complete_occurrences"], len(paths))
        blocks = {block["id"]: block for block in report["blocks"]}
        self.assertEqual(set(blocks), set(targets))
        for bid in targets:
            self.assertEqual(blocks[bid]["unique_payloads"], 1)
            self.assertEqual(blocks[bid]["variants"][0]["source_occurrences"], len(paths))
        self.assertEqual(blocks[67]["variants"][0]["raw_hex"],
                         "93115210a4f9e9036200a9fe620027ff64000000")
        self.assertEqual(blocks[5122]["variants"][0]["raw_hex"], "000000000000")
        self.assertEqual(blocks[5123]["variants"][0]["raw_hex"], "00" * 14)


if __name__ == "__main__":
    unittest.main()
