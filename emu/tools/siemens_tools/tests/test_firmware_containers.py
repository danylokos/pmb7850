#!/usr/bin/env python3
"""Service and update executable container tests."""

import zipfile
from .firmware_fixtures import *  # noqa: F401,F403



def _zip_payload(name: str, payload: bytes) -> bytes:
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w") as archive:
        archive.writestr(name, payload)
    return output.getvalue()


class ExeTests(unittest.TestCase):
    def test_service_v1_extracts_xor_payload(self) -> None:
        payload = build_xbi(writes=[(0x10, b"service")])
        executable = service_v1(payload)

        self.assertEqual(fw.detect_exe_type(executable), "service")
        self.assertEqual(fw.detect_service_exe_version(executable), 1)
        self.assertEqual(fw.extract_exe(executable), [payload])

    def test_service_info_reports_format_and_payload(self) -> None:
        payload = build_xbi(writes=[(0x10, b"service")])
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "service.exe"
            path.write_bytes(service_v1(payload))
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                status = firmware_main(["info", str(path), "--json"])

        self.assertEqual(status, 0)
        description = json.loads(stdout.getvalue())
        self.assertEqual(description["kind"], "exe")
        self.assertEqual(description["exe_type"], "service")
        self.assertEqual(description["service_format_version"], 1)
        self.assertEqual(len(description["payloads"]), 1)

    def test_legacy_service_and_xbi_trailer_use_nine_byte_marker(self) -> None:
        payload = build_xbi(writes=[(0x10, b"legacy")])
        executable = legacy_service(payload)

        self.assertEqual(fw.detect_service_exe_version(executable), 0)
        extracted = fw.extract_exe(executable)[0]
        self.assertEqual(extracted, payload + executable[-fw.SAG_JK_TRAILER_SIZE:])
        self.assertEqual(fw.convert_xbi_to_flash(extracted)[0x10:0x16], b"legacy")

    def test_service_v2_preserves_payload_index_order(self) -> None:
        payloads = [b"zero", b"one", b"two", b"three"]
        executable = service_v2(payloads)

        self.assertEqual(fw.detect_service_exe_version(executable), 2)
        self.assertEqual(fw.extract_exe(executable), payloads)

    def test_plain_and_aes_update_extract(self) -> None:
        payload = build_xbi(writes=[(0x10, b"update")])
        self.assertEqual(fw.extract_exe(update_exe(payload, False)), [payload])
        self.assertEqual(fw.extract_exe(update_exe(payload, True)), [payload])

    def test_multiple_firmware_payloads_require_selection(self) -> None:
        first = build_xbi(writes=[(0x10, b"first")])
        second = build_xbi(writes=[(0x10, b"second")])
        executable = service_v2([first, b"map", second, b"zip"])
        path = Path("multi.exe")

        with self.assertRaisesRegex(fw.FirmwareError, "multiple firmware"):
            select_convert_payload(path, executable, None)
        selected, name = select_convert_payload(path, executable, 2)
        self.assertEqual(selected, second)
        self.assertEqual(name, "T55_240101.bin")

    def test_ffsinit_info_xfs_conversion_and_direct_bin(self) -> None:
        archive = _zip_payload("Bitmap/Test.bmp", b"payload")
        executable = update_exe(archive, False)
        xfs = build_xbi(
            flash_size=0x100, model=b"C55", svn=24, update_type=7,
            writes=[(0x10, b"FFS")],
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            reference_exe = root / "FFSInit_C55_2_Test_24_0001.exe"
            reference_xfs = root / "C55_2_Test_24_0001.xfs"
            source = root / "FFSInit_C55_2_Copy_24_0001.exe"
            reference_exe.write_bytes(executable)
            reference_xfs.write_bytes(xfs)
            source.write_bytes(executable)

            info_stdout = io.StringIO()
            with contextlib.redirect_stdout(info_stdout):
                info_status = firmware_main(["info", str(source), "--json"])
            with contextlib.redirect_stdout(io.StringIO()):
                convert_status = firmware_main(["convert", str(source)])
            recovered = source.with_suffix(".xfs").read_bytes()
            output_bin = root / "ffs.bin"
            with contextlib.redirect_stdout(io.StringIO()):
                bin_status = firmware_main([
                    "convert", str(source), "--bin", "-o", str(output_bin)
                ])
            bin_data = output_bin.read_bytes()

        self.assertEqual(info_status, 0)
        description = json.loads(info_stdout.getvalue())
        self.assertEqual(description["exe_type"], "ffsinit")
        self.assertEqual(description["ffsinit"]["model"], "C55")
        self.assertEqual(description["ffsinit"]["software_version"], 24)
        self.assertEqual(convert_status, 0)
        self.assertEqual(recovered, xfs)
        self.assertEqual(bin_status, 0)
        self.assertEqual(bin_data[0x10:0x13], b"FFS")

    def test_ffsinit_without_byte_exact_reference_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = (
                Path(directory) / "FFSInit_C55_2_Unmatched_24_0001.exe"
            )
            source.write_bytes(update_exe(_zip_payload("new", b"data"), False))
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                status = firmware_main(["convert", str(source)])

        self.assertEqual(status, 1)
        self.assertIn("no byte-exact FFSInit reference", stderr.getvalue())




if __name__ == "__main__":
    unittest.main()
