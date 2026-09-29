"""Required bundled inputs must fail closed; native selection uses the same manifest."""
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools import bundled_firmware as firmware


class BundledFirmwareTests(unittest.TestCase):
    def test_all_images_match_manifest(self):
        entries = firmware.entries()
        self.assertEqual(len(entries), 36)
        self.assertEqual(sum(item["size"] for item in entries), 478150656)
        for item in entries:
            with self.subTest(image=item["image"]):
                firmware.verify(item)

    def test_missing_and_corrupt_required_input_fail(self):
        item = firmware.entry("c55")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with mock.patch.object(firmware, "ROOT", root):
                with self.assertRaisesRegex(FileNotFoundError, "Required bundled firmware missing"):
                    firmware.verify(item)
                path = root / item["image"]
                path.parent.mkdir(parents=True)
                path.write_bytes(b"corrupt")
                with self.assertRaisesRegex(ValueError, "size/hash mismatch"):
                    firmware.verify(item)

    def test_native_header_selects_python_defaults(self):
        with tempfile.TemporaryDirectory() as directory:
            header = Path(directory) / "bundled.h"
            with mock.patch("sys.argv", ["bundled_firmware", "--header", str(header)]):
                firmware.main()
            text = header.read_text()
        for model in firmware.manifest()["devices"]:
            self.assertIn(f'#define BUNDLED_{model.upper()} {json.dumps(str(firmware.image(model)))}', text)
