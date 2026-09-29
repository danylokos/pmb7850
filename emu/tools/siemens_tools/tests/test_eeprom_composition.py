from __future__ import annotations

import tempfile
import unittest
from dataclasses import replace
from pathlib import Path

from tools.siemens_tools import eeprom
from tools.siemens_tools.firmware.xbi import FirmwareError
from tools.siemens_tools.fullflash.eeprom_composition import (
    compose_a52_eeprom,
    compose_a55_eeprom,
    compose_a60_eeprom,
    compose_a62_eeprom,
    compose_c55_eeprom,
    compose_cf62_eeprom,
    compose_m55_eeprom,
    compose_mc60_eeprom,
)
from tools.siemens_tools.fullflash.eeprom_packing import (
    pack_a52_eeprom,
    pack_a55_eeprom,
    pack_a60_eeprom,
    pack_c55_eeprom,
    pack_m55_eeprom,
    pack_mc60_eeprom,
)


def write_map(path: Path, *, product: int = 130, sw: int = 24) -> None:
    sections = [
        f"[MapFileInfo]\nProduct = {product}\nSWVersion = {sw}\n",
    ]
    records = (
        (67, 2, 2, bytes.fromhex("ff" * 16 + "64000000")),
        (5001, 8, 0, b"\x12\x34"),
        (5005, 8, 1, b"\xFF" * 64),
        (5011, 8, 0, bytes(12)),
        (5372, 8, 1, b"\xFF" * 30),
    )
    for block_id, memory, version, payload in records:
        rendered = " ".join(f"0x{value:02X}" for value in payload)
        sections.append(
            f"[{block_id}] ; fixture\nMemory = {memory}\nVersion = {version}\n"
            f"DataSize = {len(payload)}\nData {{ {rendered} }}\n"
        )
    path.write_text("\n".join(sections), encoding="latin-1")


def write_m55_map(path: Path) -> None:
    sections = ["[MapFileInfo]\nProduct = 86\nSWVersion = 91\n"]
    records = (
        (67, 2, 2, bytes.fromhex("ff" * 16 + "64000000")),
        (5001, 8, 0, b"factory"),
    )
    for block_id, memory, version, payload in records:
        rendered = " ".join(f"0x{value:02X}" for value in payload)
        sections.append(
            f"[{block_id}] ; M55 fixture\nMemory = {memory}\nVersion = {version}\n"
            f"DataSize = {len(payload)}\nData {{ {rendered} }}\n"
        )
    path.write_text("\n".join(sections), encoding="latin-1")


def write_a55_map(path: Path) -> None:
    write_map(path, product=196, sw=9)


def write_a52_map(path: Path) -> None:
    write_map(path, product=226, sw=9)


def write_mc60_map(path: Path) -> None:
    write_map(path, product=132, sw=13)


def write_cf62_map(path: Path) -> None:
    write_map(path, product=228, sw=7)


def write_a60_map(path: Path) -> None:
    write_map(path, product=39, sw=27)


def write_a62_map(path: Path) -> None:
    write_map(path, product=231, sw=7)


def write_c55_donor(
    path: Path, *, include_blocks: tuple[int, ...] = (67, 5005, 5006),
) -> None:
    payloads = {
        67: bytes(range(20)),
        5005: bytes([0xA5]) * 64,
        5006: bytes(range(34)),
    }
    classes = {67: 2, 5005: 8, 5006: 8}
    versions = {67: 2, 5005: 1, 5006: 0}
    # Keep EELITE populated so automatic region-base derivation remains valid
    # when block 67 is the intentionally omitted donor candidate.
    blocks = {3: b"\x00"}
    blocks.update({block_id: payloads[block_id] for block_id in include_blocks})
    classes[3] = 2
    versions[3] = 0
    packed, _description = pack_c55_eeprom(
        blocks,
        {block_id: classes[block_id] for block_id in blocks},
        {block_id: versions[block_id] for block_id in blocks},
    )
    path.write_bytes(packed)


def write_m55_donor(
    path: Path, *, include_blocks: tuple[int, ...] = (67, 5005, 5006),
) -> None:
    payloads = {
        67: bytes(range(20)),
        5005: bytes([0xA5]) * 64,
        5006: bytes(range(34)),
    }
    all_classes = {67: 2, 5005: 8, 5006: 8}
    all_versions = {67: 2, 5005: 1, 5006: 0}
    blocks = {3: b"\x00"}
    blocks.update({block_id: payloads[block_id] for block_id in include_blocks})
    classes = {3: 2}
    classes.update({block_id: all_classes[block_id] for block_id in include_blocks})
    versions = {3: 0}
    versions.update({block_id: all_versions[block_id] for block_id in include_blocks})
    packed, _description = pack_m55_eeprom(blocks, classes, versions)
    image = bytearray(b"\xFF" * 0x1000000)
    image[0xFC0000:0xFF0000] = packed
    path.write_bytes(image)


def write_a55_donor(
    path: Path,
    *,
    include_blocks: tuple[int, ...] = (67, 5005, 5006),
) -> None:
    payloads = {
        67: bytes(range(20)),
        5005: bytes([0xA5]) * 64,
        5006: bytes(range(34)),
        5121: bytes(range(56)),
    }
    all_classes = {67: 2, 5005: 8, 5006: 8, 5121: 8}
    all_versions = {67: 2, 5005: 1, 5006: 0, 5121: 0}
    blocks = {3: b"\x00"}
    blocks.update({block_id: payloads[block_id] for block_id in include_blocks})
    classes = {3: 2}
    classes.update({block_id: all_classes[block_id] for block_id in include_blocks})
    versions = {3: 0}
    versions.update({block_id: all_versions[block_id] for block_id in include_blocks})
    packed, _description = pack_a55_eeprom(blocks, classes, versions)
    path.write_bytes(packed)


def write_a52_donor(
    path: Path, *, include_blocks: tuple[int, ...] = (67, 5005, 5006),
) -> None:
    payloads = {67: bytes(range(20)), 5005: bytes([0xA5]) * 64, 5006: bytes(range(34))}
    classes = {3: 2, 67: 2, 5005: 8, 5006: 8}
    versions = {3: 0, 67: 2, 5005: 1, 5006: 0}
    blocks = {3: b"\x00", **{i: payloads[i] for i in include_blocks}}
    packed, _description = pack_a52_eeprom(
        blocks,
        {i: classes[i] for i in blocks},
        {i: versions[i] for i in blocks},
    )
    path.write_bytes(packed)


def write_mc60_donor(
    path: Path, *, include_blocks: tuple[int, ...] = (67, 5005, 5006),
) -> None:
    payloads = {67: bytes(range(20)), 5005: bytes([0xA5]) * 64, 5006: bytes(range(34))}
    blocks = {3: b"\x00", **{i: payloads[i] for i in include_blocks}}
    classes = {3: 2, 67: 2, 5005: 8, 5006: 8}
    versions = {3: 0, 67: 2, 5005: 1, 5006: 0}
    packed, _description = pack_mc60_eeprom(
        blocks, {i: classes[i] for i in blocks}, {i: versions[i] for i in blocks},
    )
    image = bytearray(b"\xFF" * 0x1000000)
    image[0xFC0000:0xFF0000] = packed
    path.write_bytes(image)


def write_a60_donor(
    path: Path, *, include_blocks: tuple[int, ...] = (67, 5005, 5006),
) -> None:
    payloads = {67: bytes(range(20)), 5005: bytes([0xA5]) * 64, 5006: bytes(range(34))}
    blocks = {3: b"\x00", **{i: payloads[i] for i in include_blocks}}
    classes = {3: 2, 67: 2, 5005: 8, 5006: 8}
    versions = {3: 0, 67: 2, 5005: 1, 5006: 0}
    packed, _description = pack_a60_eeprom(
        blocks, {i: classes[i] for i in blocks}, {i: versions[i] for i in blocks},
    )
    image = bytearray(b"\xFF" * 0x800000)
    image[0x7C0000:0x7F0000] = packed
    path.write_bytes(image)


class EepromCompositionTests(unittest.TestCase):
    def test_all_profiles_use_versioned_generic_battery_record(self) -> None:
        payload = eeprom.encode_battery_calibration(
            eeprom.CEMU_DEFAULT_BATTERY_CALIBRATION
        )
        for profile in eeprom.EEPROM_PROFILES.values():
            with self.subTest(profile=profile.name):
                records = [
                    record for record in profile.records
                    if record.block_id == 67
                ]
                self.assertEqual(len(records), 1)
                record = records[0]
                self.assertEqual(record.payload, payload)
                self.assertEqual(
                    (record.memory_class, record.version), (2, 2),
                )
                self.assertEqual(
                    record.payload_kind, "synthesized-cemu-battery-v1",
                )
                self.assertEqual(record.observed_bytes, 0)
                self.assertIn("emulator calibration policy", record.provenance)
                self.assertNotIn(67, profile.donor_blocks)
        self.assertEqual(
            eeprom.A65_EMULATOR_MINIMAL_PROFILE.donor_blocks,
            (306, 5005, 5006, 5436, 5437),
        )
        for profile in eeprom.EEPROM_PROFILES.values():
            if profile is eeprom.A65_EMULATOR_MINIMAL_PROFILE:
                continue
            self.assertEqual(profile.donor_blocks, (5005, 5006))

    def test_a52_cloned_profile_is_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            map_path, donor_path = root / "factory.map", root / "donor.bin"
            write_a52_map(map_path)
            write_a52_donor(donor_path)
            profile = replace(
                eeprom.A52_EMULATOR_MINIMAL_PROFILE,
                donor_sha256=None,
                map_sha256=None,
            )
            first = compose_a52_eeprom(
                map_path, software_version=9, profile_override=profile,
                donor_path=donor_path, imei="35361800468244", fsn=0xC84E9718,
            )
            second = compose_a52_eeprom(
                map_path, software_version=9, profile_override=profile,
                donor_path=donor_path, imei="35361800468244", fsn=0xC84E9718,
            )
        self.assertEqual(first, second)
        self.assertEqual(first.manifest["model"], "A52")
        self.assertEqual(first.manifest["source_counts"]["donor-fullflash"], 2)
        self.assertEqual(first.manifest["source_counts"]["profile"], 18)
        self.assertEqual(
            [first.image[x + 0x1C] for x in (0, 0x20000, 0x40000)],
            [0xA0, 0xA0, 0xA0],
        )

    def test_a55_profile_identity_native_packing_and_determinism(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            map_path = root / "factory.map"
            donor_path = root / "donor.bin"
            write_a55_map(map_path)
            write_a55_donor(donor_path)
            profile = replace(
                eeprom.A55_EMULATOR_MINIMAL_PROFILE,
                donor_sha256=None,
                map_sha256=None,
            )
            first = compose_a55_eeprom(
                map_path, software_version=9, profile_override=profile,
                donor_path=donor_path, imei="35283800375615", fsn=0xAF4E6A0B,
            )
            second = compose_a55_eeprom(
                map_path, software_version=9, profile_override=profile,
                donor_path=donor_path, imei="35283800375615", fsn=0xAF4E6A0B,
            )

        self.assertEqual(first, second)
        self.assertEqual(len(first.image), 0x60000)
        self.assertEqual(first.manifest["model"], "A55")
        self.assertEqual(first.manifest["record_count"], 26)
        self.assertEqual(first.manifest["source_counts"], {
            "donor-fullflash": 2,
            "factory-map": 2,
            "generated-identity": 4,
            "profile": 18,
        })
        self.assertEqual(
            [first.image[0x1C], first.image[0x2001C], first.image[0x4001C]],
            [0xA0, 0xA3, 0xA3],
        )
        records = {record["id"]: record for record in first.manifest["records"]}
        self.assertEqual(records[67]["source"], "profile")
        self.assertEqual(
            records[67]["payload_kind"], "synthesized-cemu-battery-v1",
        )
        for block_id in (5005, 5006):
            self.assertEqual(records[block_id]["source"], "donor-fullflash")
        for block_id in (
            1, 2, 55, 57, 75, 144, 5121, 5122, 5123, 5180, 5181,
            5255, 5256, 5257, 5258, 5259, 5372,
        ):
            self.assertEqual(records[block_id]["source"], "profile")
            self.assertEqual(records[block_id]["payload_sha256"], __import__("hashlib").sha256(bytes(records[block_id]["length"])).hexdigest())
        self.assertEqual(records[5011]["source"], "factory-map")

    def test_a55_profile_rejects_unpinned_map(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            map_path = root / "factory.map"
            donor_path = root / "donor.bin"
            write_a55_map(map_path)
            write_a55_donor(donor_path)
            profile = replace(
                eeprom.A55_EMULATOR_MINIMAL_PROFILE,
                donor_sha256=None,
                map_sha256="0" * 64,
            )
            with self.assertRaisesRegex(FirmwareError, "does not match profile"):
                compose_a55_eeprom(
                    map_path, software_version=9, profile_override=profile,
                    donor_path=donor_path,
                )

    def test_profile_identity_and_packing_are_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "factory.map"
            donor_path = root / "donor.bin"
            write_map(path)
            write_c55_donor(donor_path)
            profile = replace(
                eeprom.C55_EMULATOR_MINIMAL_PROFILE,
                donor_sha256=None,
                map_sha256=None,
            )
            first = compose_c55_eeprom(
                path, software_version=24,
                profile_override=profile, donor_path=donor_path,
                imei="35335000894548", fsn=0xA35F2F28,
            )
            second = compose_c55_eeprom(
                path, software_version=24,
                profile_override=profile, donor_path=donor_path,
                imei="35335000894548", fsn=0xA35F2F28,
            )

        self.assertEqual(first, second)
        self.assertEqual(len(first.image), 0x60000)
        self.assertEqual(first.manifest["record_count"], 17)
        self.assertEqual(first.manifest["source_counts"], {
            "donor-fullflash": 2,
            "factory-map": 1,
            "generated-identity": 4,
            "profile": 8,
            "profile-normalization": 2,
        })
        self.assertEqual(first.manifest["donor"]["blocks"], [5005, 5006])
        self.assertEqual(first.manifest["omitted_blocks"], [])
        self.assertEqual(first.manifest["identity"]["fsn"], "A35F2F28")
        records = {record["id"]: record for record in first.manifest["records"]}
        self.assertEqual((records[5011]["length"], records[5011]["version"]), (36, 2))
        self.assertEqual((records[5372]["length"], records[5372]["version"]), (30, 0))
        self.assertEqual(records[67]["source"], "profile")
        self.assertEqual(
            records[67]["payload_sha256"],
            "c4247e2de00dc5e625f29fe4653b513b1e576ecbc597f9672614c640ccc70d8f",
        )
        for block_id in (5005, 5006):
            self.assertEqual(records[block_id]["source"], "donor-fullflash")
        inventory = eeprom.load_dump_blocks(
            first.image, eeprom.find_eeprom_region(first.image)
        )
        self.assertEqual(
            inventory[67].payload,
            eeprom.encode_battery_calibration(
                eeprom.CEMU_DEFAULT_BATTERY_CALIBRATION
            ),
        )
        self.assertEqual(inventory[5005].payload, bytes([0xA5]) * 64)
        self.assertEqual(inventory[5006].payload, bytes(range(34)))
        self.assertEqual(
            (inventory[5005].memory_class, inventory[5005].version), (8, 1),
        )
        identity = eeprom.generate_identity_bundle("35335000894548", 0xA35F2F28)
        self.assertEqual(
            inventory[5009].payload.hex().upper(), identity["blocks"]["5009"],
        )
        self.assertEqual(eeprom.recreate_imei(inventory[5009].payload)[0], "35335000894548")

    def test_map_only_preserves_payload_and_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "factory.map"
            write_map(path)
            result = compose_c55_eeprom(path, software_version=24)
        self.assertEqual(result.manifest["record_count"], 5)
        self.assertEqual(result.manifest["normalizations"], [])
        records = {record["id"]: record for record in result.manifest["records"]}
        self.assertEqual((records[5011]["length"], records[5011]["version"]), (12, 0))
        self.assertEqual((records[5372]["length"], records[5372]["version"]), (30, 1))

    def test_programmatic_profile_override_supports_ablation(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "factory.map"
            donor_path = root / "donor.bin"
            write_map(path)
            write_c55_donor(donor_path)
            profile = eeprom.C55_EMULATOR_MINIMAL_PROFILE
            reduced = replace(
                profile,
                donor_blocks=(5005,),
                donor_sha256=None,
                map_sha256=None,
            )
            result = compose_c55_eeprom(
                path, software_version=24, profile_override=reduced,
                donor_path=donor_path,
                normalization_block_ids=frozenset({5372}),
            )
        self.assertNotIn(5006, {record["id"] for record in result.manifest["records"]})
        records = {record["id"]: record for record in result.manifest["records"]}
        self.assertEqual((records[5011]["length"], records[5011]["version"]), (12, 0))

    def test_rejects_wrong_map_scope_and_partial_identity(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            wrong_product = root / "wrong-product.map"
            wrong_sw = root / "wrong-sw.map"
            write_map(wrong_product, product=131)
            write_map(wrong_sw, sw=23)
            with self.assertRaisesRegex(FirmwareError, "not C55"):
                compose_c55_eeprom(wrong_product, software_version=24)
            with self.assertRaisesRegex(FirmwareError, "does not match"):
                compose_c55_eeprom(wrong_sw, software_version=24)
            valid = root / "valid.map"
            write_map(valid)
            with self.assertRaisesRegex(FirmwareError, "both IMEI and FSN"):
                compose_c55_eeprom(
                    valid, software_version=24, imei="35335000894548",
                )

    def test_builtin_profiles_require_all_three_donor_blocks(self) -> None:
        cases = (
            (
                "a62", write_a62_map, write_a60_donor, compose_a62_eeprom,
                eeprom.A62_EMULATOR_MINIMAL_PROFILE, 7,
            ),
            (
                "a60", write_a60_map, write_a60_donor, compose_a60_eeprom,
                eeprom.A60_EMULATOR_MINIMAL_PROFILE, 27,
            ),
            (
                "cf62", write_cf62_map, write_mc60_donor, compose_cf62_eeprom,
                eeprom.CF62_EMULATOR_MINIMAL_PROFILE, 7,
            ),
            (
                "mc60", write_mc60_map, write_mc60_donor, compose_mc60_eeprom,
                eeprom.MC60_EMULATOR_MINIMAL_PROFILE, 13,
            ),
            (
                "a52", write_a52_map, write_a52_donor, compose_a52_eeprom,
                eeprom.A52_EMULATOR_MINIMAL_PROFILE, 9,
            ),
            (
                "a55", write_a55_map, write_a55_donor, compose_a55_eeprom,
                eeprom.A55_EMULATOR_MINIMAL_PROFILE, 9,
            ),
            (
                "c55", write_map, write_c55_donor, compose_c55_eeprom,
                eeprom.C55_EMULATOR_MINIMAL_PROFILE, 24,
            ),
            (
                "m55", write_m55_map, write_m55_donor, compose_m55_eeprom,
                eeprom.M55_EMULATOR_MINIMAL_PROFILE, 91,
            ),
        )
        for model, map_writer, donor_writer, compose, profile, sw in cases:
            for missing in profile.donor_blocks:
                with self.subTest(model=model, missing=missing):
                    with tempfile.TemporaryDirectory() as directory:
                        root = Path(directory)
                        map_path = root / "factory.map"
                        donor_path = root / "donor.bin"
                        map_writer(map_path)
                        donor_writer(
                            donor_path,
                            include_blocks=tuple(
                                block_id for block_id in profile.donor_blocks
                                if block_id != missing
                            ),
                        )
                        with self.assertRaisesRegex(
                            FirmwareError, "lacks profile blocks",
                        ):
                            compose(
                                map_path, software_version=sw,
                                profile_override=replace(
                                    profile, donor_sha256=None, map_sha256=None,
                                ),
                                donor_path=donor_path,
                            )

    def test_m55_map_identity_geometry_and_determinism(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "m55.map"
            write_m55_map(path)
            first = compose_m55_eeprom(
                path, software_version=91,
                imei="35202600729559", fsn=0xC8AAE55F,
            )
            second = compose_m55_eeprom(
                path, software_version=91,
                imei="35202600729559", fsn=0xC8AAE55F,
            )
        self.assertEqual(first, second)
        self.assertEqual(len(first.image), 0x30000)
        self.assertEqual(first.manifest["model"], "M55")
        self.assertEqual(first.manifest["record_count"], 6)
        self.assertEqual(first.manifest["source_counts"], {
            "factory-map": 2, "generated-identity": 4,
        })
        self.assertEqual(
            [first.image[0x8C], first.image[0x1008C], first.image[0x2008C]],
            [0xA1, 0xAA, 0xA9],
        )
        self.assertEqual(first.manifest["packing"]["bank_size"], 0x10000)

    def test_m55_donor_selection_and_provenance(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            map_path = root / "m55.map"
            donor_path = root / "donor.bin"
            write_m55_map(map_path)
            write_m55_donor(donor_path)
            profile = eeprom.EepromProfile(
                "m55-test", 86, (91,), (), "test", "test",
                (
                    eeprom.ProfileRecord(
                        167, bytes(348), 2, 1, "synthesized-zero", 0, "test",
                    ),
                ),
                (), (67, 5006),
            )
            result = compose_m55_eeprom(
                map_path, software_version=91, profile_override=profile,
                donor_path=donor_path,
                imei="35202600729559", fsn=0xC8AAE55F,
            )
        records = {record["id"]: record for record in result.manifest["records"]}
        self.assertEqual(result.manifest["donor"]["blocks"], [67, 5006])
        self.assertEqual(result.manifest["source_counts"], {
            "donor-fullflash": 2,
            "factory-map": 1,
            "generated-identity": 4,
            "profile": 1,
        })
        self.assertEqual(records[67]["source"], "donor-fullflash")
        self.assertEqual(records[5006]["payload_sha256"], __import__("hashlib").sha256(bytes(range(34))).hexdigest())
        self.assertEqual(records[167]["source"], "profile")

    def test_m55_rejects_missing_and_mismatched_donor(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            map_path = root / "m55.map"
            donor_path = root / "donor.bin"
            write_m55_map(map_path)
            write_m55_donor(donor_path, include_blocks=(67, 5005))
            profile = eeprom.EepromProfile(
                "m55-test", 86, (91,), (), "test", "test", (), (),
                (67, 5006),
            )
            with self.assertRaisesRegex(FirmwareError, "lacks profile blocks"):
                compose_m55_eeprom(
                    map_path, software_version=91, profile_override=profile,
                    donor_path=donor_path,
                )
            mismatched = replace(profile, donor_blocks=(67,), donor_sha256="0" * 64)
            with self.assertRaisesRegex(FirmwareError, "does not match profile"):
                compose_m55_eeprom(
                    map_path, software_version=91, profile_override=mismatched,
                    donor_path=donor_path,
                )

    def test_m55_donor_requires_profile_and_required_donor(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            map_path = root / "m55.map"
            donor_path = root / "donor.bin"
            write_m55_map(map_path)
            write_m55_donor(donor_path)
            with self.assertRaisesRegex(FirmwareError, "requires a named EEPROM profile"):
                compose_m55_eeprom(
                    map_path, software_version=91, donor_path=donor_path,
                )
            profile = eeprom.EepromProfile(
                "m55-test", 86, (91,), (), "test", "test", (), (), (67,),
            )
            with self.assertRaisesRegex(FirmwareError, "requires --eeprom-donor"):
                compose_m55_eeprom(
                    map_path, software_version=91, profile_override=profile,
                )


if __name__ == "__main__":
    unittest.main()
