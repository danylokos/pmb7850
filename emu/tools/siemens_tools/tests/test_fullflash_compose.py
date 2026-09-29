from __future__ import annotations

import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from tools.siemens_tools import fullflash as ff
from tools.siemens_tools import layout as lt
from tools.siemens_tools.fullflash.compose import compose_manifest
from tools.siemens_tools.fullflash.corpus import build_corpus
from tools.siemens_tools.tests.test_fullflash_corpus import _synthetic_sources


class FullflashCompositionTests(unittest.TestCase):
    def test_ordered_dump_roles_custom_and_erased_slices(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sources, original = _synthetic_sources(root)
            corpus_root = root / "community"
            corpus = build_corpus(
                [sources],
                lt.load_layout("C55"),
                write_splits=True,
                corpus_root=corpus_root,
            )
            artifact = next(
                item for item in corpus["artifacts"]
                if item["kind"] == "complete-fullflash"
            )
            recipe = corpus_root / artifact["recipe"]["path"]
            layout = lt.load_layout("C55").layout
            eeprom = layout.region("EEPROM")
            ee_fs = layout.region("EE_FS")

            baseline = bytes([0xA5]) * layout.length
            baseline_path = root / "baseline.bin"
            baseline_path.write_bytes(baseline)
            custom_eeprom = bytes(
                (index * 29 + 7) & 0xFF for index in range(eeprom.length)
            )
            custom_path = root / "custom-eeprom.bin"
            custom_path.write_bytes(custom_eeprom)
            manifest = root / "composition.json"
            document = {
                "schema": "siemens-fullflash-composition",
                "schema_version": 1,
                "layout": {"name": "C55"},
                "operations": [
                    {
                        "kind": "custom",
                        "path": baseline_path.name,
                        "range": {
                            "from": 0,
                            "to_exclusive": layout.length,
                            "length": layout.length,
                        },
                        "sha256": hashlib.sha256(baseline).hexdigest(),
                    },
                    {
                        "kind": "dump-recipe",
                        "recipe": recipe.relative_to(root).as_posix(),
                        "roles": ["BCORE"],
                    },
                    {
                        "kind": "custom",
                        "path": custom_path.name,
                        "role": "EEPROM",
                        "sha256": hashlib.sha256(custom_eeprom).hexdigest(),
                    },
                    {"kind": "erased", "role": "EE_FS"},
                ],
            }
            manifest.write_text(json.dumps(document))
            first = compose_manifest(manifest)
            second = compose_manifest(manifest)
            self.assertEqual(first.sha256, second.sha256)
            expected = bytearray(baseline)
            bcore = layout.region("BCORE")
            expected[bcore.offset:bcore.end] = original[bcore.offset:bcore.end]
            expected[eeprom.offset:eeprom.end] = custom_eeprom
            expected[ee_fs.offset:ee_fs.end] = b"\xFF" * ee_fs.length
            self.assertEqual(first.image, bytes(expected))
            self.assertEqual(
                first.image[0x330:0x340], original[0x330:0x340]
            )
            self.assertGreaterEqual(len(first.overlaps), 3)

    def test_custom_region_can_use_partial_only_donor_payload(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sources, original = _synthetic_sources(root)
            layout = lt.load_layout("C55").layout
            eeprom = layout.region("EEPROM")
            damaged = bytearray(original)
            damaged[:4] = bytes.fromhex("00 80 34 12")
            donor = bytes(
                (index * 17 + 3) & 0xFF for index in range(eeprom.length)
            )
            damaged[eeprom.offset:eeprom.end] = donor
            (sources / "partial-donor.bin").write_bytes(damaged)
            corpus_root = root / "community"
            corpus = build_corpus(
                [sources], lt.load_layout("C55"),
                write_splits=True, corpus_root=corpus_root,
            )
            partial = next(
                item for item in corpus["artifacts"]
                if item["kind"] == "partial-fullflash"
            )
            occurrence = next(
                item for item in corpus["occurrences"]
                if item["source_id"] == partial["id"]
                and item["role"] == "EEPROM"
            )
            self.assertFalse(any(
                item["source_kind"] == "complete-fullflash"
                and item["role"] == "EEPROM"
                and item["sha256"] == occurrence["sha256"]
                for item in corpus["occurrences"]
            ))

            baseline = bytes([0xA5]) * layout.length
            (root / "baseline.bin").write_bytes(baseline)
            manifest = root / "composition.json"
            manifest.write_text(json.dumps({
                "schema": "siemens-fullflash-composition",
                "schema_version": 1,
                "layout": {"name": "C55"},
                "operations": [
                    {
                        "kind": "custom",
                        "path": "baseline.bin",
                        "range": {
                            "from": 0,
                            "to_exclusive": layout.length,
                            "length": layout.length,
                        },
                        "sha256": hashlib.sha256(baseline).hexdigest(),
                    },
                    {
                        "kind": "custom",
                        "path": (Path("community") / occurrence["payload_path"]).as_posix(),
                        "role": "EEPROM",
                        "sha256": occurrence["sha256"],
                    },
                ],
            }))
            result = compose_manifest(manifest)
            self.assertEqual(result.image[eeprom.offset:eeprom.end], donor)

    def test_complete_dump_recipe_and_uncovered_rejection(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sources, original = _synthetic_sources(root)
            corpus_root = root / "community"
            corpus = build_corpus(
                [sources],
                lt.load_layout("C55"),
                write_splits=True,
                corpus_root=corpus_root,
            )
            recipe = corpus_root / next(
                item["recipe"]["path"] for item in corpus["artifacts"]
                if item["kind"] == "complete-fullflash"
            )
            manifest = root / "composition.json"
            base = {
                "schema": "siemens-fullflash-composition",
                "schema_version": 1,
                "layout": {"name": "C55"},
                "operations": [{
                    "kind": "dump-recipe",
                    "recipe": recipe.relative_to(root).as_posix(),
                }],
            }
            manifest.write_text(json.dumps(base))
            self.assertEqual(compose_manifest(manifest).image, original)

            base["operations"][0]["roles"] = ["BCORE"]
            manifest.write_text(json.dumps(base))
            with self.assertRaisesRegex(
                lt.FirmwareError, "uncovered ranges"
            ):
                compose_manifest(manifest)


if __name__ == "__main__":
    unittest.main()
