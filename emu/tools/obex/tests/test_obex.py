#!/usr/bin/env python3
from __future__ import annotations

import asyncio
import io
import os
import pty
import select
import sys
import tempfile
import unittest
from pathlib import Path

EMU_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(EMU_ROOT))

from tools.obex import codec
from tools.obex.cli import run_shell
from tools.obex import resolve_remote_path
from tools.obex.client import ObexFilesystemClient, SIEMENS_FFS_TARGET
from tools.obex.errors import ProtocolError
from tools.obex.listing import parse_folder_listing
from tools.obex.models import ClientConfig, EntryKind, RemotePath
from tools.obex.streams import MemorySink, MemorySource
from tools.obex.transport import SerialTransport


class ScriptedTransport:
    def __init__(self) -> None:
        self.incoming = bytearray()
        self.writes: list[tuple[str, bytes]] = []
        self.opened = False

    async def open(self) -> None:
        self.opened = True

    async def close(self) -> None:
        self.opened = False

    async def write_all(self, data: bytes, phase: str) -> None:
        self.writes.append((phase, data))
        if phase.startswith("at-"):
            self.incoming.extend(b"\r\nOK\r\n")
        elif phase == "obex-connect":
            headers = codec.uint32_header(codec.CONNECTION_ID, 0x10203040)
            headers += codec.bytes_header(codec.WHO, SIEMENS_FFS_TARGET)
            self.incoming.extend(codec.encode_packet(codec.SUCCESS, b"\x10\x00\x04\x00", headers))
        elif phase == "obex-setpath":
            self.incoming.extend(codec.encode_packet(codec.SUCCESS))
        elif phase == "obex-get":
            xml = b'<?xml version="1.0"?><folder-listing><folder name="pics" modified="20260101T010203"/><file name="a.txt" size="3" owner="phone"/></folder-listing>'
            self.incoming.extend(codec.encode_packet(
                codec.SUCCESS,
                codec.uint32_header(codec.LENGTH, len(xml)),
                codec.bytes_header(codec.END_BODY, xml),
            ))
        elif phase == "obex-put":
            code = codec.SUCCESS if data[0] == codec.PUT_FINAL else codec.CONTINUE
            self.incoming.extend(codec.encode_packet(code))
        elif phase in ("obex-delete", "obex-disconnect", "obex-abort"):
            self.incoming.extend(codec.encode_packet(codec.SUCCESS))

    async def read_exactly(self, length: int, phase: str, timeout=None) -> bytes:
        if len(self.incoming) < length:
            raise AssertionError(f"script has no {length} bytes for {phase}")
        data = bytes(self.incoming[:length])
        del self.incoming[:length]
        return data


@unittest.skipUnless(os.name == "posix", "PTY transport test requires POSIX")
class SerialTransportPtyTests(unittest.IsolatedAsyncioTestCase):
    async def test_bidirectional_real_pty_without_worker_thread(self) -> None:
        master_fd, slave_fd = pty.openpty()
        path = os.ttyname(slave_fd)
        transport = SerialTransport(ClientConfig(path, timeout=1.0))
        try:
            await transport.open()
            await transport.write_all(b"host-to-phone", "test-write")
            readable, _, _ = select.select([master_fd], [], [], 1.0)
            self.assertTrue(readable)
            self.assertEqual(os.read(master_fd, 64), b"host-to-phone")

            os.write(master_fd, b"phone-to-host")
            self.assertEqual(
                await transport.read_exactly(13, "test-read"),
                b"phone-to-host",
            )
        finally:
            await transport.close()
            os.close(slave_fd)
            os.close(master_fd)


class CodecTests(unittest.TestCase):
    def test_packet_and_all_header_widths(self) -> None:
        raw = codec.bytes_header(codec.TYPE, b"text/plain\0")
        raw += bytes((0x90, 7))
        raw += codec.uint32_header(codec.LENGTH, 0x12345678)
        packet = codec.encode_packet(codec.GET_FINAL, raw)
        self.assertEqual(codec.decode_prefix(packet[:3]), (codec.GET_FINAL, len(packet)))
        headers = codec.parse_headers(packet[3:])
        self.assertEqual(headers[0].value, b"text/plain\0")
        self.assertEqual(headers[1].value, 7)
        self.assertEqual(headers[2].value, 0x12345678)

    def test_rejects_bad_lengths(self) -> None:
        with self.assertRaises(ProtocolError):
            codec.decode_prefix(b"\xa0\x00\x02")
        with self.assertRaises(ProtocolError):
            codec.parse_headers(b"\x42\x00\x09x")

    def test_remote_paths_are_absolute_and_safe(self) -> None:
        path = RemotePath("A:/pics/a.txt")
        self.assertEqual(str(path), "A:\\pics\\a.txt")
        self.assertEqual(str(path.parent), "A:\\pics")
        with self.assertRaises(ValueError):
            RemotePath("pics/a.txt")
        with self.assertRaises(ValueError):
            RemotePath("A:/../a.txt")

    def test_shell_paths_support_relative_components_and_trailing_separators(self) -> None:
        cwd = RemotePath("A:/Bitmap/Sub")
        self.assertEqual(str(resolve_remote_path("../", cwd)), "A:\\Bitmap")
        self.assertEqual(str(resolve_remote_path("/Sounds/", cwd)), "A:\\Sounds")
        self.assertEqual(str(resolve_remote_path("A:\\Bitmap\\", cwd)), "A:\\Bitmap")
        with self.assertRaisesRegex(ValueError, "above the drive root"):
            resolve_remote_path("../../../", cwd)

    def test_folder_listing_preserves_unknown_metadata(self) -> None:
        entries = parse_folder_listing(
            b'<folder-listing><file name="a" size="12" custom="x"/></folder-listing>',
            RemotePath("A:"),
        )
        self.assertEqual(entries[0].kind, EntryKind.FILE)
        self.assertEqual(entries[0].size, 12)
        self.assertEqual(entries[0].attributes["custom"], "x")

    def test_folder_listing_honors_declared_encoding(self) -> None:
        xml = (
            '<?xml version="1.0" encoding="iso-8859-1"?>'
            '<folder-listing><file name="caf\xe9.bmp"/></folder-listing>'
        )
        entries = parse_folder_listing(xml.encode("latin-1"), RemotePath("A:"))
        self.assertEqual(entries[0].name, "caf\xe9.bmp")

    def test_folder_listing_decodes_measured_c55_name_marker(self) -> None:
        xml = (
            b'<?xml version="1.0"?><folder-listing>'
            b'<file name="\x1fM\xc3\x90\xc2\xb0trix.bmp"/>'
            b'<file name="\x1f\xc3\x90\xc2\x9c\xc3\x90\xc2\x90\xc3\x90\xc2\xa0\xc3\x90\xc2\xa2.bmp"/>'
            b'</folder-listing>'
        )
        entries = parse_folder_listing(xml, RemotePath("A:/Bitmap"))
        self.assertEqual([entry.name for entry in entries], ["M\u0430trix.bmp", "\u041c\u0410\u0420\u0422.bmp"])
        self.assertEqual(entries[0].attributes["name"], "M\u0430trix.bmp")

    def test_folder_listing_rejects_malformed_marked_name(self) -> None:
        xml = b'<folder-listing><file name="\x1f\xc3\x83"/></folder-listing>'
        with self.assertRaisesRegex(ProtocolError, "Siemens encoded"):
            parse_folder_listing(xml, RemotePath("A:"))


class ClientTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self) -> None:
        self.transport = ScriptedTransport()
        self.client = ObexFilesystemClient(
            ClientConfig("fake", sqwe0_settle=0), transport=self.transport
        )
        self.info = await self.client.connect()

    async def asyncTearDown(self) -> None:
        await self.client.close()

    async def test_connect_and_list_are_domain_objects(self) -> None:
        self.assertEqual(self.info.connection_id, 0x10203040)
        self.assertEqual(self.info.peer_max_packet, 1024)
        entries = await self.client.list(RemotePath("A:"))
        self.assertEqual([(entry.kind.value, entry.name) for entry in entries],
                         [("folder", "pics"), ("file", "a.txt")])
        self.assertEqual(entries[1].attributes["owner"], "phone")
        self.assertFalse(any(phase == "obex-setpath" for phase, _ in self.transport.writes))

    async def test_drive_is_session_root_and_only_children_use_setpath(self) -> None:
        await self.client.list(RemotePath("A:/pics"))
        packets = [raw for phase, raw in self.transport.writes if phase == "obex-setpath"]
        self.assertEqual(len(packets), 1)
        headers = codec.parse_headers(packets[0][5:])
        self.assertEqual(headers[1].identifier, codec.NAME)
        self.assertEqual(headers[1].value, "pics".encode("utf-16-be") + b"\0\0")

        for drive in ("B:", "C:", "D:"):
            with self.subTest(drive=drive), self.assertRaisesRegex(
                ValueError,
                f"unsupported Siemens OBEX filesystem root '{drive}'.*"
                "no host-side drive selector is proven",
            ):
                await self.client.list(RemotePath(drive))

    async def test_streaming_upload_chunks_and_reports_result(self) -> None:
        data = bytes(range(256)) * 9
        result = await self.client.upload(RemotePath("A:/large.bin"), MemorySource(data))
        self.assertEqual(result.bytes_transferred, len(data))
        packets = [raw for phase, raw in self.transport.writes if phase == "obex-put"]
        self.assertGreater(len(packets), 2)
        self.assertEqual(packets[-1][0], codec.PUT_FINAL)
        self.assertTrue(all(packet[0] == codec.PUT for packet in packets[:-1]))

    async def test_download_writes_to_abstract_sink(self) -> None:
        sink = MemorySink()
        result = await self.client.download(RemotePath("A:/a.txt"), sink)
        self.assertGreater(result.bytes_transferred, 3)
        self.assertIn(b"folder-listing", sink.data)

    async def test_mutations_use_library_not_cli_state(self) -> None:
        await self.client.mkdir(RemotePath("A:/new"))
        await self.client.delete(RemotePath("A:/old.txt"))
        await self.client.rmdir(RemotePath("A:/old"))
        phases = [phase for phase, _ in self.transport.writes]
        self.assertIn("obex-setpath", phases)
        self.assertEqual(phases.count("obex-delete"), 2)

    async def test_mkdir_keeps_navigation_state_in_component_form(self) -> None:
        await self.client.mkdir(RemotePath("A:/new"))
        await self.client.list(RemotePath("A:/new"))
        packets = [raw for phase, raw in self.transport.writes if phase == "obex-setpath"]
        self.assertEqual(len(packets), 1)

    async def test_shell_reuses_connection_and_continues_after_command_error(self) -> None:
        commands = io.StringIO(
            "ls A:/\n"
            "ls\n"
            "cd pics\n"
            "pwd\n"
            "ls 'A:\\pics\\'\n"
            "cd ..\n"
            "pwd\n"
            "status\n"
            "bogus\n"
            "pwd\n"
            "quit\n"
        )
        output = io.StringIO()
        errors = io.StringIO()
        result = await run_shell(
            self.client, input_stream=commands, output_stream=output, error_stream=errors
        )
        self.assertEqual(result, 0)
        self.assertEqual(
            sum(phase == "obex-connect" for phase, _ in self.transport.writes), 1
        )
        self.assertEqual(sum(phase == "obex-get" for phase, _ in self.transport.writes), 5)
        self.assertIn("A:\\pics", output.getvalue())
        self.assertGreaterEqual(output.getvalue().count("A:\\\n"), 2)
        self.assertIn('"connection_id": 270544960', output.getvalue())
        self.assertIn("unknown command 'bogus'", errors.getvalue())

    async def test_shell_dispatches_transfer_and_mutation_commands(self) -> None:
        with tempfile.TemporaryDirectory(prefix="obex-shell-") as directory:
            source = Path(directory) / "source.bin"
            destination = Path(directory) / "destination.bin"
            source.write_bytes(b"payload")
            commands = io.StringIO(
                f"put {source} A:/uploaded.bin\n"
                f"get --force A:/a.txt {destination}\n"
                "mkdir A:/new\n"
                "delete A:/old.txt\n"
                "rmdir A:/old\n"
                "quit\n"
            )
            result = await run_shell(
                self.client,
                input_stream=commands,
                output_stream=io.StringIO(),
                error_stream=io.StringIO(),
            )
            self.assertEqual(result, 0)
            self.assertIn(b"folder-listing", destination.read_bytes())

        phases = [phase for phase, _ in self.transport.writes]
        self.assertIn("obex-put", phases)
        self.assertIn("obex-get", phases)
        self.assertEqual(phases.count("obex-delete"), 2)


if __name__ == "__main__":
    unittest.main()
