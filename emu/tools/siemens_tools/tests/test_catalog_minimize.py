#!/usr/bin/env python3
"""Community-catalog official-backing minimization tests."""

from __future__ import annotations

import hashlib
import json
import os
import tempfile
import unittest
from pathlib import Path

from tools.siemens_tools.fullflash.catalog_backing import (
    validate_official_backing,
)
from tools.siemens_tools.fullflash.catalog_minimize import (
    _target_path,
    minimize_catalog,
    plan_minimization,
)
from tools.siemens_tools.fullflash.reconstruct import reconstruct_recipe
from tools.siemens_tools.firmware.xbi import FirmwareError


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


class CatalogMinimizeTests(unittest.TestCase):
    def _fixture(self, root: Path, *, schema_version: int = 14) -> dict[str, object]:
        dump_root = root / "community"
        official_root = root / "official"
        dump_root.mkdir()
        official_root.mkdir()
        matched = b"official-backed payload"
        unmatched = b"dump-only payload"
        matched_hash = _sha256(matched)
        unmatched_hash = _sha256(unmatched)
        name = f"{matched_hash[:12]}.bin"
        unmatched_name = f"{unmatched_hash[:12]}.bin"

        official_path = f"24/lg91/11/t9/{name}"
        official_unknown = f"unknown/unknown/unknown/t9/{name}"
        official_other = f"24/lg1/11/t9/{name}"
        (official_root / official_path).parent.mkdir(parents=True)
        (official_root / official_path).write_bytes(matched)
        for relative in (official_unknown, official_other):
            link = official_root / relative
            link.parent.mkdir(parents=True)
            link.symlink_to(os.path.relpath(
                official_root / official_path, link.parent
            ))

        dump_path = f"24/lg91/11/t9/{name}"
        dump_unknown = f"unknown/unknown/unknown/t9/{name}"
        (dump_root / dump_path).parent.mkdir(parents=True)
        (dump_root / dump_path).write_bytes(matched)
        dump_alias = dump_root / dump_unknown
        dump_alias.parent.mkdir(parents=True)
        dump_alias.symlink_to(os.path.relpath(
            dump_root / dump_path, dump_alias.parent
        ))

        unmatched_path = f"24/lg91/11/langpack/{unmatched_name}"
        unmatched_alias = f"25/lg91/11/langpack/{unmatched_name}"
        (dump_root / unmatched_path).parent.mkdir(parents=True)
        (dump_root / unmatched_path).write_bytes(unmatched)
        alias = dump_root / unmatched_alias
        alias.parent.mkdir(parents=True)
        alias.symlink_to(os.path.relpath(
            dump_root / unmatched_path, alias.parent
        ))

        layout = {
            "name": "C55",
            "base": 0x800000,
            "length": len(matched),
            "catalog_sha256": "a" * 64,
        }
        official = {
            "schema": "siemens-official-corpus",
            "schema_version": 9,
            "layout": layout,
            "regions": [{
                "role": "T9",
                "variants": [{
                    "sha256": matched_hash,
                    "size": len(matched),
                    "erased": False,
                    "payload_path": official_path,
                    "symlink_paths": [official_other, official_unknown],
                    "occurrences": [],
                }],
            }],
            "packages": [],
        }
        dump = {
            "schema": "siemens-community-fullflash-corpus",
            "schema_version": schema_version,
            "layout": layout,
            "summary": {
                "regular_payload_files": 2,
                "regular_payload_bytes": len(matched) + len(unmatched),
                "official_backed_variants": 0,
                "official_backed_paths": 0,
                "official_backed_bytes": 0,
                "payload_symlinks": 2,
                "payload_paths": 4,
            },
            "regions": [
                {
                    "role": "T9",
                    "variants": [{
                        "sha256": matched_hash,
                        "size": len(matched),
                        "erased": False,
                        "payload_path": dump_path,
                        "symlink_paths": [dump_unknown],
                        "occurrences": ["occurrence-001"],
                    }],
                },
                {
                    "role": "LangPack",
                    "variants": [{
                        "sha256": unmatched_hash,
                        "size": len(unmatched),
                        "erased": False,
                        "payload_path": unmatched_path,
                        "symlink_paths": [unmatched_alias],
                        "occurrences": [],
                    }],
                },
            ],
            "artifacts": [{
                "id": "source-001",
                "metadata": {"patch_analysis": {
                    "baseline": {"payload_path": dump_path},
                    "identified_patches": [{"patch_id": "fixture"}],
                }},
            }],
            "occurrences": [{
                "id": "occurrence-001",
                "source_id": "source-001",
                "role": "T9",
                "sha256": matched_hash,
                "payload_path": dump_path,
                "patch_analysis": {"identified_patch_ids": ["fixture"]},
            }],
        }
        dump_catalog = dump_root / "catalog.json"
        official_catalog = official_root / "catalog.json"
        _write_json(dump_catalog, dump)
        _write_json(official_catalog, official)
        return {
            "dump_root": dump_root,
            "official_root": official_root,
            "dump_catalog": dump_catalog,
            "official_catalog": official_catalog,
            "dump_path": dump_path,
            "dump_unknown": dump_unknown,
            "official_path": official_path,
            "official_unknown": official_unknown,
            "unmatched_path": unmatched_path,
            "unmatched_alias": unmatched_alias,
            "matched": matched,
            "matched_hash": matched_hash,
        }

    def test_target_ranking_prioritizes_sw_then_lg_then_t9_then_path(
        self,
    ) -> None:
        source = "24/lg91/11/t9/source.bin"
        self.assertEqual(
            _target_path(source, [
                "25/lg91/11/t9/sw-mismatch.bin",
                "24/lg1/11/t9/lg-mismatch.bin",
                "24/lg91/10/t9/t9-mismatch.bin",
            ], "T9"),
            "24/lg91/10/t9/t9-mismatch.bin",
        )
        self.assertEqual(
            _target_path(source, [
                "24/lg91/10/t9/z.bin",
                "24/lg91/10/t9/a.bin",
            ], "T9"),
            "24/lg91/10/t9/a.bin",
        )

    def test_obsolete_schema_is_rejected_without_changes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = self._fixture(Path(directory), schema_version=12)
            catalog = fixture["dump_catalog"]
            before = catalog.read_bytes()
            alias = fixture["dump_root"] / fixture["dump_unknown"]
            before_alias = os.readlink(alias)

            for dry_run in (True, False):
                with self.subTest(dry_run=dry_run), self.assertRaisesRegex(
                    FirmwareError, "schema 14",
                ):
                    minimize_catalog(
                        catalog, fixture["official_catalog"], dry_run=dry_run,
                    )
            self.assertEqual(catalog.read_bytes(), before)
            self.assertEqual(os.readlink(alias), before_alias)

    def test_apply_ranks_targets_preserves_unmatched_alias_and_is_idempotent(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = self._fixture(Path(directory))
            before = json.loads(fixture["dump_catalog"].read_text())
            report = minimize_catalog(
                fixture["dump_catalog"], fixture["official_catalog"]
            )
            self.assertFalse(report["dry_run"])
            dump_root = fixture["dump_root"]
            canonical = dump_root / fixture["dump_path"]
            unknown = dump_root / fixture["dump_unknown"]
            self.assertEqual(
                Path(os.path.abspath(canonical.parent / os.readlink(canonical))),
                Path(os.path.abspath(
                    fixture["official_root"] / fixture["official_path"]
                )),
            )
            self.assertEqual(
                Path(os.path.abspath(unknown.parent / os.readlink(unknown))),
                Path(os.path.abspath(
                    fixture["official_root"] / fixture["official_unknown"]
                )),
            )
            self.assertTrue(
                (fixture["official_root"] / fixture["official_unknown"]).is_symlink()
            )
            self.assertFalse(
                (dump_root / fixture["unmatched_path"]).is_symlink()
            )
            self.assertTrue(
                (dump_root / fixture["unmatched_alias"]).is_symlink()
            )

            document = json.loads(fixture["dump_catalog"].read_text())
            self.assertEqual(document["artifacts"], before["artifacts"])
            self.assertEqual(document["occurrences"], before["occurrences"])
            self.assertEqual(document["schema_version"], 14)
            self.assertEqual(document["summary"]["regular_payload_files"], 1)
            self.assertEqual(document["summary"]["official_backed_variants"], 1)
            self.assertEqual(document["summary"]["official_backed_paths"], 2)
            self.assertEqual(
                validate_official_backing(document, fixture["dump_catalog"]),
                {"variants": 1, "paths": 2, "bytes": len(fixture["matched"])},
            )
            first_catalog = fixture["dump_catalog"].read_bytes()
            first_mtime = fixture["dump_catalog"].stat().st_mtime_ns
            first_links = (os.readlink(canonical), os.readlink(unknown))
            repeated = minimize_catalog(
                fixture["dump_catalog"], fixture["official_catalog"]
            )
            self.assertEqual(repeated["matching_variants"], 1)
            self.assertEqual(repeated["newly_backed_variants"], 0)
            self.assertEqual(repeated["bytes_removed"], 0)
            self.assertEqual(fixture["dump_catalog"].read_bytes(), first_catalog)
            self.assertEqual(
                fixture["dump_catalog"].stat().st_mtime_ns, first_mtime
            )
            self.assertEqual(
                (os.readlink(canonical), os.readlink(unknown)), first_links
            )

    def test_reconstruction_accepts_only_validated_external_backing(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = self._fixture(Path(directory))
            minimize_catalog(fixture["dump_catalog"], fixture["official_catalog"])
            recipe = fixture["dump_root"] / "fullflashes" / "fixture.json"
            payload = fixture["matched"]
            digest = fixture["matched_hash"]
            _write_json(recipe, {
                "schema": "siemens-fullflash-recipe",
                "schema_version": 4,
                "fullflash": {"size": len(payload), "sha256": digest},
                "slices": [{
                    "order": 0,
                    "role": "T9",
                    "payload_path": fixture["dump_path"],
                    "normalization": None,
                    "sha256": digest,
                    "source_sha256": digest,
                    "size": len(payload),
                    "source_range": {
                        "from": 0,
                        "to_exclusive": len(payload),
                        "length": len(payload),
                    },
                }],
            })
            self.assertEqual(reconstruct_recipe(recipe), payload)

            (fixture["official_root"] / fixture["official_path"]).write_bytes(
                b"corrupt"
            )
            with self.assertRaisesRegex(FirmwareError, "size or hash mismatch"):
                reconstruct_recipe(recipe)

    def test_corruption_and_unsafe_paths_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = self._fixture(Path(directory))
            (fixture["official_root"] / fixture["official_path"]).write_bytes(
                b"corrupt"
            )
            with self.assertRaisesRegex(FirmwareError, "size or hash mismatch"):
                plan_minimization(
                    fixture["dump_catalog"], fixture["official_catalog"]
                )

        with tempfile.TemporaryDirectory() as directory:
            fixture = self._fixture(Path(directory))
            document = json.loads(fixture["dump_catalog"].read_text())
            document["regions"][0]["variants"][0]["payload_path"] = "../bad.bin"
            _write_json(fixture["dump_catalog"], document)
            with self.assertRaisesRegex(FirmwareError, "unsafe community payload"):
                plan_minimization(
                    fixture["dump_catalog"], fixture["official_catalog"]
                )


if __name__ == "__main__":
    unittest.main()
