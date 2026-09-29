"""Command-line adapter for :mod:`tools.obex`."""

from __future__ import annotations

import argparse
import asyncio
import json
import shlex
import sys
from typing import TextIO

from .client import ObexFilesystemClient
from .errors import ObexError, TransportError, OverwriteRequired
from .models import ClientConfig, FileEntry, RemotePath, TransferProgress, resolve_remote_path

from .streams import FileSink, FileSource

_SHELL_HELP = """commands:
  ls [--json] [REMOTE]       list a directory (default: current directory)
  cd REMOTE                  change the current remote directory
  pwd                        print the current remote directory
  get [--force] REMOTE LOCAL download a file
  put [--overwrite] LOCAL REMOTE  upload or confirm replacement
  delete REMOTE              delete a file
  mkdir REMOTE               create a directory
  rmdir REMOTE               remove a directory
  status                     print OBEX connection information
  help                       show this help
  quit | exit                close the session
"""


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="python -m tools.obex", description="Siemens x55 serial OBEX filesystem client")
    parser.add_argument("--port", required=True, help="serial device or PTY")
    parser.add_argument("--baud", type=int, default=19200)
    parser.add_argument("--timeout", type=float, default=40.0)
    parser.add_argument("--wire-trace", metavar="PATH", help="append JSONL wire trace")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("connect", help="probe the Siemens filesystem target")
    commands.add_parser("shell", help="run several filesystem commands in one OBEX session")
    listing = commands.add_parser("ls", help="list a phone directory")
    listing.add_argument("path")
    listing.add_argument("--json", action="store_true")
    get = commands.add_parser("get", help="download a phone file")
    get.add_argument("phone_path")
    get.add_argument("local_file")
    get.add_argument("--force", action="store_true")
    put = commands.add_parser("put", help="upload a local file")
    put.add_argument("local_file")
    put.add_argument("phone_path")
    put.add_argument("--overwrite", action="store_true", help="confirm replacement after metadata recheck")
    for name in ("delete", "mkdir", "rmdir"):
        command = commands.add_parser(name)
        command.add_argument("path")
    return parser


async def _progress(value: TransferProgress) -> None:
    total = "?" if value.total is None else str(value.total)
    print(f"{value.operation}: {value.completed}/{total} {value.state}", file=sys.stderr)


def _print_entries(entries: list[FileEntry], *, json_output: bool, output: TextIO) -> None:
    if json_output:
        print(json.dumps(
            [entry.to_dict() for entry in entries], indent=2, sort_keys=True,
            ensure_ascii=False,
        ), file=output)
        return
    for entry in entries:
        size = "-" if entry.size is None else str(entry.size)
        print(f"{entry.kind.value:6} {size:>10} {entry.name}", file=output)


def _require_args(command: str, args: list[str], minimum: int, maximum: int | None = None) -> None:
    maximum = minimum if maximum is None else maximum
    if not minimum <= len(args) <= maximum:
        expected = str(minimum) if minimum == maximum else f"{minimum}..{maximum}"
        raise ValueError(f"{command} expects {expected} argument(s); try 'help'")


async def _download(client: ObexFilesystemClient, remote: RemotePath, local: str,
                    *, force: bool) -> None:
    sink = FileSink(local, force=force)
    try:
        await client.download(remote, sink, progress=_progress)
    finally:
        sink.close()


async def _upload(client: ObexFilesystemClient, local: str, remote: RemotePath,
                  *, overwrite=False, input_stream=None, output_stream=None) -> None:
    input_stream = sys.stdin if input_stream is None else input_stream
    output_stream = sys.stderr if output_stream is None else output_stream
    source = FileSource(local)
    try:
        try:
            await client.upload(remote, source, progress=_progress)
        except OverwriteRequired as exc:
            if not overwrite and getattr(input_stream, "isatty", lambda: False)():
                print(f"Overwrite {remote}? [y/N] ", end="", flush=True, file=output_stream)
                overwrite = input_stream.readline().strip().lower() in ("y", "yes")
            if not overwrite:
                raise
            # No source bytes were consumed before the confirmation exception.
            # A second metadata change requires a new user decision.
            await client.upload(remote, source, progress=_progress, confirmation_token=exc.token)
    finally:
        source.close()


async def _run_shell_command(client: ObexFilesystemClient, words: list[str],
                             cwd: RemotePath, output: TextIO, input_stream=None) -> tuple[RemotePath, bool]:
    command, args = words[0].lower(), words[1:]
    if command in ("quit", "exit"):
        _require_args(command, args, 0)
        return cwd, True
    if command == "help":
        _require_args(command, args, 0)
        print(_SHELL_HELP, end="", file=output)
    elif command == "status":
        _require_args(command, args, 0)
        if client.info is None:
            raise TransportError("OBEX session is not connected")
        print(json.dumps(client.info.to_dict(), sort_keys=True), file=output)
    elif command == "pwd":
        _require_args(command, args, 0)
        print(f"{cwd}\\" if cwd.parent is None else str(cwd), file=output)
    elif command == "ls":
        json_output = False
        if "--json" in args:
            args = list(args)
            args.remove("--json")
            json_output = True
        _require_args(command, args, 0, 1)
        target = resolve_remote_path(args[0], cwd) if args else cwd
        _print_entries(await client.list(target), json_output=json_output, output=output)
    elif command == "cd":
        _require_args(command, args, 1)
        target = resolve_remote_path(args[0], cwd)
        await client.list(target)
        cwd = target
    elif command == "get":
        force = False
        if "--force" in args:
            args = list(args)
            args.remove("--force")
            force = True
        _require_args(command, args, 2)
        await _download(client, resolve_remote_path(args[0], cwd), args[1], force=force)
    elif command == "put":
        overwrite = "--overwrite" in args
        args = [arg for arg in args if arg != "--overwrite"]
        _require_args(command, args, 2)
        await _upload(client, args[0], resolve_remote_path(args[1], cwd),
                      overwrite=overwrite, input_stream=input_stream, output_stream=output)
    elif command in ("delete", "mkdir", "rmdir"):
        _require_args(command, args, 1)
        method = getattr(client, command)
        await method(resolve_remote_path(args[0], cwd))
    else:
        raise ValueError(f"unknown command {command!r}; try 'help'")
    return cwd, False


async def run_shell(client: ObexFilesystemClient, *, input_stream: TextIO | None = None,
                    output_stream: TextIO | None = None,
                    error_stream: TextIO | None = None) -> int:
    input_stream = sys.stdin if input_stream is None else input_stream
    output_stream = sys.stdout if output_stream is None else output_stream
    error_stream = sys.stderr if error_stream is None else error_stream
    cwd = RemotePath("A:")
    interactive = bool(getattr(input_stream, "isatty", lambda: False)())
    while True:
        if interactive:
            print(f"obex:{cwd}> ", end="", flush=True, file=output_stream)
        line = input_stream.readline()
        if not line:
            return 0
        try:
            words = shlex.split(line, comments=False, posix=True)
            if not words:
                continue
            cwd, finished = await _run_shell_command(client, words, cwd, output_stream, input_stream)
            if finished:
                return 0
        except (TimeoutError, TransportError) as exc:
            print(f"error: {exc}", file=error_stream)
            return 1
        except (ObexError, OSError, ValueError) as exc:
            print(f"error: {exc}", file=error_stream)


async def run(args: argparse.Namespace) -> int:
    config = ClientConfig(args.port, args.baud, args.timeout, wire_trace=args.wire_trace)
    client = ObexFilesystemClient(config)
    try:
        info = await client.connect()
        if args.command == "connect":
            print(json.dumps(info.to_dict(), sort_keys=True))
        elif args.command == "shell":
            return await run_shell(client)
        elif args.command == "ls":
            entries = await client.list(RemotePath(args.path))
            _print_entries(entries, json_output=args.json, output=sys.stdout)
        elif args.command == "get":
            await _download(client, RemotePath(args.phone_path), args.local_file, force=args.force)
        elif args.command == "put":
            await _upload(client, args.local_file, RemotePath(args.phone_path), overwrite=args.overwrite)
        else:
            method = getattr(client, args.command)
            await method(RemotePath(args.path))
        return 0
    finally:
        await client.close()


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return asyncio.run(run(args))
    except (ObexError, TimeoutError, OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
