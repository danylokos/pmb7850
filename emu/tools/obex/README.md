# Siemens x55 OBEX client

tools.obex is the asynchronous client for the Siemens Line55 filesystem
service. The CLI and browser bridge share its session implementation. Use an
existing serial device or a PTY exposed with the [EMU host's](../../README.md)
--serial-pty option. A PTY path in emu/shots/ is used below; start the
emulator and create the shots/ directory before running the client.

## CLI

From emu/, after installing requirements.txt into the repository virtual
environment:

~~~sh
mkdir -p shots
PYTHONPATH=. ../.venv/bin/python -m tools.obex --port shots/phone-asc0 connect
PYTHONPATH=. ../.venv/bin/python -m tools.obex --port shots/phone-asc0 ls 'A:\' --json
PYTHONPATH=. ../.venv/bin/python -m tools.obex --port shots/phone-asc0 get 'A:\file.txt' shots/download.txt
PYTHONPATH=. ../.venv/bin/python -m tools.obex --port shots/phone-asc0 put shots/upload.txt 'A:\file.txt'
PYTHONPATH=. ../.venv/bin/python -m tools.obex --port shots/phone-asc0 mkdir 'A:\scratch'
PYTHONPATH=. ../.venv/bin/python -m tools.obex --port shots/phone-asc0 delete 'A:\file.txt'
PYTHONPATH=. ../.venv/bin/python -m tools.obex --port shots/phone-asc0 rmdir 'A:\scratch'
~~~

The default baud rate is 19200; use --baud for another serial setup.
--timeout sets the operation timeout. --wire-trace shots/obex-wire.jsonl
appends timestamped JSONL with exact transmitted and received bytes.
Use --help on the main command or a subcommand for current arguments.

For multiple operations in one connection:

~~~sh
PYTHONPATH=. ../.venv/bin/python -m tools.obex --port shots/phone-asc0 shell
~~~

The shell accepts ls, cd, pwd, get, put, delete, mkdir, rmdir, status,
help, and quit/exit. It resolves relative paths against its current remote
directory. Slash and backslash separators, trailing separators, and . and
.. are normalized without traversal above A:\. Shell quoting preserves
spaces and backslashes; piped stdin uses the same commands without prompts.

## Library API

The public exports are in tools.obex. Transfers use UploadSource and
DownloadSink protocols rather than host paths, so callers can stream
without buffering entire files.

~~~python
from tools.obex import ClientConfig, MemorySink, ObexFilesystemClient, RemotePath

async with ObexFilesystemClient(ClientConfig("shots/phone-asc0")) as phone:
    entries = await phone.list(RemotePath("A:\\"))
    sink = MemorySink()
    await phone.download(RemotePath("A:\\file.txt"), sink)
~~~

One client owns one transport and serializes whole operations, including
preliminary listings and replacement. Do not run two clients on one port.
RemotePath accepts absolute A: paths and normalizes separators;
resolve_remote_path(value, cwd) handles relative CLI paths. Other drive
roots are rejected. A long-lived controller should retain one connected
client rather than opening a new session per operation.

upload(path, source, confirmation_token=None, progress=None) raises
OverwriteRequired with .path and .token before reading source bytes if the
target file exists. Repeat with that single-use token after user approval.
The token is bound to the client, normalized path, and listed kind, size,
and modification time. Changed metadata requires another confirmation;
an existing directory raises PathConflict. Confirmed replacement deletes
the old file before PUT, so a later failure can leave the old file absent.
The metadata check cannot detect an external edit that preserves size and
modification time.

CLI and shell put --overwrite LOCAL REMOTE perform the same recheck.
Without --overwrite, an interactive terminal prompts; noninteractive
calls report an error. get --force REMOTE LOCAL overwrites a local destination.

download(path, sink, metadata=callback, progress=callback) invokes the
async metadata callback with FileEntry before body bytes. Progress
callbacks receive TransferProgress. on_mutation receives MutationEvent
after successful DELETE, PUT, or mkdir; replacement reports DELETE and
upload separately. Callbacks run under the operation lock and must not
recursively call client operations.

state is a ConnectionState and error is a string or None. Cancellation
while waiting for the lock leaves the session intact. Active cancellation,
transport failure, timeout, or malformed protocol retires the session
and its confirmation tokens before releasing the lock. No OBEX ABORT is
injected. Queued calls fail against the retired session; connect(retry=True)
explicitly reconnects. invalidate(reason), abort(), and close() use the
same teardown policy. A reconnect does not force handset firmware back
to AT mode.

tools.obex.errors.ProtocolError describes AT/OBEX framing and differs
from tools.ui_protocol.ProtocolError for the native UI protocol.

## Protocol and limits

The client uses AT^SQWE=0, a one-second default quiet interval,
AT^SQWE=3, and raw OBEX with target UUID
6b01cb31-4106-11d4-9a77-0050da3f471f. ClientConfig.sqwe0_settle
controls that interval; it is client/emulator pacing calibration, not a
proven physical-protocol requirement. BFB/SecurityLayer is not selected
automatically.

The exposed filesystem root is A:\. A separate internal FFS_B region in
some images does not establish host OBEX access to B:. The library, CLI,
and browser reject non-A roots instead of relabeling A: or synthesizing
access to flash.

OBEX availability depends on the exact firmware image and its boot state.
The bundled generated images are startup fixtures, not end-to-end OBEX
qualification. Stock C55 SW24 does not naturally accept SQWE3; the
default-off c55-sqwe3 firmware patch is a diagnostic way to exercise its
firmware-owned server. A52/A55 remain unqualified at their current
execution frontiers. Do not treat a patch or debugger setup as a natural
firmware pass.

C55 SW24 does not currently return to AT-command mode after OBEX
Disconnect, a wait, and +++. Use shell for multiple operations in one
session, then start a fresh emulator session after leaving it. Physical
cable/modem-control teardown is not modeled.

## Tests

From the pmb7850/ root:

~~~sh
cd emu
../.venv/bin/python -m unittest discover -s tools/obex/tests -t .
~~~
