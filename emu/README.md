# EMU host

EMU is the stable host application for PMB7850 handsets. It validates
images, prepares immutable inputs, runs CEMU or supervised QEMU, and owns
transports, artifacts, and normalized output. CEMU is the default engine.
The C structs and static archives are internal interfaces, not a plugin ABI.

## Build this checkout

From the pmb7850/ root, use Python 3.14, a C compiler, GNU Make, pkg-config,
and the GLib/GIO, pixman, and zlib development libraries:

~~~sh
git submodule update --init emu/vendor/carquet qemu gdb
python3.14 -m venv .venv
.venv/bin/python -m pip install -r emu/requirements.txt
make -C emu deps
make -C emu
make -C cemu test
emu/bin/emu --help
~~~

The checkout includes pinned QEMU/GDB sources and 36 generated fullflash
images. [Firmware inputs](../firmware/README.md) gives their hashes,
manifest usage, and validation limits. Packaged images are immutable.

QEMU additionally needs Ninja and its upstream build dependencies. Run
qemu-deps once to fetch the pinned Meson subprojects; QEMU profile
configuration is offline afterward. GDB needs a C++17 compiler, Expat,
GMP, MPFR, ncurses, Texinfo, Bison, and Flex.

~~~sh
make -C emu qemu-deps
make -C emu qemu-release
make -C emu gdb
make -C emu test-gdb-build
~~~

The GDB helper builds a remote-only C166 target under gdb/build/c166/ and
does not install it. It reuses a matching managed configuration; use a fresh
GDB_BUILD_DIR under this checkout for an incompatible configuration.
test-gdb-build checks an existing binary and never builds one. QEMU profiles
are qemu/build/debug/ and qemu/build/release/; qemu-layout rejects other
entries in qemu/build/.

For the full standalone check, install the test dependencies and browser:

~~~sh
.venv/bin/python -m pip install -r emu/requirements-dev.txt
.venv/bin/python -m playwright install --with-deps chromium
make -C emu test-standalone
~~~

This verifies bundled hashes, native and tool tests, and bounded startup for
all 36 images in both engines. Its report is
emu/shots/standalone-startup/report.json. Startup success does not qualify
boot, UI, or CEMU/QEMU parity.

## Run

From the repository root:

~~~sh
emu/bin/emu run firmware/C55/c55sw249111-556677-d9258d0fe59a.bin \
  --limit 8m --monitor --summary
emu/bin/emu run firmware/C55/c55sw249111-556677-d9258d0fe59a.bin \
  --engine qemu --qemu-binary qemu/build/release/qemu-system-c166
emu/bin/emu run --help
~~~

QEMU runs continuously until stopped unless a supported run control ends
it. The canonical command is bin/emu run; omitting --engine selects CEMU.
bin/cemu and bin/cemu_inst are internal development artifacts. The host
selects CEMU's instrumented profile for diagnostics; there is no public
profile switch. Options are capability-gated before engine startup.

| Area | Main options |
|---|---|
| Input | --device, --fsn, --imei, --eeprom-overlay, --sim, --synth, --patch, --from-snapshot |
| Run control | --limit, --batch-idle, --battery-level, --battery-charging, --benchmark-json |
| Transport | --ui-socket, --serial-pty |
| Diagnostics | --monitor, --summary, --show-writes, --trace, --drcov, --lcd-frames, --lcd-ddram-frames |
| Persistence | --snapshot, --snapshot-at, --snapshot-full-bins, --dump-flash |
| Debugging | -c/--command, --script, -i/--interactive, --gdb, --gdb-binary |

Image metadata selects a device unless --device NAME overrides board wiring;
the override does not change the image's software/language IDs or flash
engine. --fsn XXXXXXXX supplies a known handset FSN without replacing logical
EEPROM. On AM29 devices, --imei DIGITS can add its customer SecSi mirror;
it requires --fsn. Built-in identity is deterministic emulator policy.
--sim attaches a deterministic card profile. Use --synth list and --patch
list to inspect diagnostic behaviors and audited startup patches.

QEMU is a supervised child. The host validates physical chip roles, sizes,
and permitted NOR models, stages private chip files, and removes them on
teardown. The source image is not mutated. QEMU uses QMP/QOM for control and
statistics, peer D-Bus for display, and a chardev for ASC0 where supported.
The host bridges these to the same UI and serial interfaces as CEMU.

## Artifacts and tools

Generated traces, reports, screenshots, and dumps belong under a task-named
emu/shots/ directory. --label NAME selects shots/NAME/ relative to the
working directory. --trace list shows selectors without booting; bare
--trace captures all. --drcov writes coverage/cov.drcov. --snapshot writes
snapshot/; --dump-flash writes the physical main array at stop. LCD switches
write displayed or raw controller frames. See the [CEMU guide](../cemu/README.md)
for CEMU diagnostics and the [trace guide](tools/cemu_trace/README.md) for
Parquet queries and schemas.

Run the Python tools from emu/ (or set PYTHONPATH=emu elsewhere):

~~~sh
cd emu
../.venv/bin/python -m tools.siemens_tools --help
../.venv/bin/python -m tools.obex --help
../.venv/bin/python -m tools.web_ui --help
../.venv/bin/python -m tools.cemu_trace --help
../.venv/bin/python tools/diff_drcov.py --help
../.venv/bin/python tools/benchmark.py --help
~~~

The [Siemens Tools](tools/siemens_tools/README.md),
[OBEX client](tools/obex/README.md), and
[qualification runner](tools/qualification/README.md) guides own their
command details. make -C emu test runs host contracts and tool suites;
make -C emu test-tools runs the tools alone. make -C emu bin/recover_fsn
builds the standalone recovery tool.

Session manifests use emu-session-manifest. Schema 1 is frozen; schema 2
records the source hash, detected metadata, selected device, physical chips,
ordered storage operations, engine and capabilities, effective options,
transport setup, result, and artifacts. Source preparation and artifact
finalization preserve source-image immutability.

## QEMU qualification

The versioned matrix uses firmware/manifest.json. It records each probe
failure and exit status; admission to the matrix does not establish parity.
C55 has separate strict gates. Select devices, gates, and output with the
Make variables below; reports stay under emu/shots/.

~~~sh
make -C emu qemu-matrix QEMU_MATRIX_DEVICES=all QEMU_MATRIX_GATES=no-sim
make -C emu qemu-matrix QEMU_MATRIX_DEVICES=all QEMU_MATRIX_GATES=perf \
  QEMU_MATRIX_ROOT=shots/my-matrix
make -C emu qemu-perf QEMU_PERF_CPU=3
~~~

The perf gate compares a CEMU milestone with QEMU samples using its
per-device policy. qemu-perf measures throughput but has no performance
qualification threshold for the bundled images. Select a CPU available on
the host. The [qualification guide](tools/qualification/README.md) lists
runner arguments, strict/probe modes, and retained report formats.

GDB gates require existing GDB_BINARY and QEMU_BINARY executables. They do
not build them or silently skip missing binaries. Defaults point to the
project's GDB and release QEMU profile; output variables select a directory
under emu/shots/.

| Make target | Scope | Output override |
|---|---|---|
| test-gdb-build | GDB version, C166 target, XML, command-file smoke test | GDB_BUILD_DIR |
| test-gdb-registers | Register interface and mapped aliases | GDB_REGISTER_OUTPUT |
| test-gdb-disassembly | Static instruction decoding | GDB_DISASSEMBLY_OUTPUT |
| test-gdb-execution | Stepping, code breakpoints, execution lifecycle | GDB_EXECUTION_OUTPUT |
| test-gdb-memory | Memory and watchpoint behavior | GDB_MEMORY_OUTPUT |
| test-gdb | Bounded native command scripting; not an aggregate gate | GDB_SCRIPTING_OUTPUT |
| test-gdb-handsets | C55/M55 firmware debugger sessions | GDB_HANDSET_OUTPUT |

~~~sh
make -C emu test-gdb-registers
make -C emu test-gdb-execution \
  QEMU_BINARY="$PWD/qemu/build/debug/qemu-system-c166" \
  GDB_EXECUTION_OUTPUT=shots/my-gdb-execution
make -C emu test-gdb-memory
~~~

The memory/watchpoint gate retains failures: an extra QEMU SIGTRAP after a
stepped watchpoint and stale MMIO-backed code translation. Reliable
watchpoint step/resume and execution of modified MMIO code remain blocked.
The other gates have their own bounds and results; a passing register or
execution gate does not waive memory failures. Native GDB command examples
are in [tools/qualification/gdb/examples](tools/qualification/gdb/examples/).

## Debuggers and transports

CEMU uses its own command debugger. QEMU uses native C166 GDB. Both keep UI
and serial clients responsive while firmware runs or pauses. From emu/:

~~~sh
mkdir -p shots
./bin/emu run ../firmware/C55/c55sw249111-556677-d9258d0fe59a.bin \
  -i --ui-socket shots/phone.sock \
  --serial-pty shots/phone-asc0
./bin/emu run ../firmware/C55/c55sw249111-556677-d9258d0fe59a.bin \
  --engine qemu -i \
  --ui-socket shots/phone.sock --serial-pty shots/phone-asc0
./bin/emu run ../firmware/C55/c55sw249111-556677-d9258d0fe59a.bin \
  --engine qemu -c 'info registers'
./bin/emu run ../firmware/C55/c55sw249111-556677-d9258d0fe59a.bin \
  --engine qemu --gdb 1234
~~~

The -i QEMU form launches the project GDB in the current terminal, paused
at reset. -c passes a native GDB command unchanged; it is repeatable.
--script loads a native command file. Without -i, GDB runs in batch mode.
--gdb PORT exposes QEMU's remote stub at 127.0.0.1:PORT, also paused at
reset, for an external GDB process. PORT must be 1..65535. Use
target remote 127.0.0.1:PORT in GDB. --gdb-binary PATH selects another
existing C166 GDB executable for one managed run.

CEMU commands are semicolon-separated and use their own syntax; see its
[debugger reference](../cemu/README.md#debugger-and-serial-transport).
The CEMU debugger currently bypasses CLI --limit, deferred trace triggers,
and custom monitor thresholds; use cont MAX or step N for bounded work.
QEMU GDB supports register inspection, static disassembly, ordinary
stepping, and code breakpoints. Inferior calls and software breakpoint
patching are unsupported; unwinding stops at the current frame. Debugger
state edits make a run diagnostic rather than a faithful boot baseline.

The [UI protocol](include/emu_ui.h) is version 13 and exposes display,
keypad, runtime statistics, and serial history. --serial-pty PATH owns
ASC0 and requires a path that does not
exist. The [OBEX guide](tools/obex/README.md) documents a serial client.
Both engines retain UI and PTY polling while a debugger pauses the guest.
Closing the session releases held keys, drains final output, and cleans up
private files.

## Snapshots and current limits

CEMU and QEMU snapshots are engine-specific. QEMU saves on Ctrl+C,
SIGTERM, or a UI stop:

~~~sh
cd emu
./bin/emu run ../firmware/C55/c55sw249111-556677-d9258d0fe59a.bin \
  --engine qemu --snapshot --label saved
./bin/emu run --engine qemu --from-snapshot shots/saved/snapshot
~~~

QEMU snapshot/ contains snapshot.json, vmstate.bin, and flash-N.bin for
each physical NOR chip. Restore checks the original fullflash hash and
native format version. The original path must still exist, or supply the
same image at a new path before the options. SIM and patches are inherited;
conflicting settings fail. Restore resumes guest state and instruction
count, while host serial history, UI ownership, and playback connections
start fresh. SIGKILL and crashes cannot save state. QEMU cannot honor
exact --snapshot-at instruction boundaries.

QEMU does not implement every CEMU control. Exact --limit, --snapshot-at,
--snapshot-full-bins, CEMU's command debugger, monitor/DRcov,
--benchmark-json, synthetic GSM, raw LCD DDRAM, and unsupported trace
selectors are rejected before
child creation. Battery controls are admitted on all twelve boards, but
their shared calibration does not establish identical charging physics.
Only trace records supplied by native QEMU with real icount/PC are
normalized; absent events are not fabricated. Device and gate results in
the retained matrix define the current parity boundary.

## Host interfaces

The host lives in emu/src/. src/adapters/cemu/ bridges the opaque CEMU core;
src/adapters/qemu/ supervises QMP, D-Bus display, ASC0, images, and traces.
The host owns image/product selection, immutable storage planning, runtime
identity, transports, normalized traces, generic DRcov output, manifests,
and PNG/GIF lifecycles. The CPU, SoC, board wiring, and peripheral behavior
remain engine-owned.

[emu_serial.h](include/emu_serial.h) exposes invocation-owned, append-only
raw serial history.
Terminal, PTY, and UI socket consumers have independent subscriptions and
64-bit cursors; range reads do not advance delivery. Views are borrowed
until append/destruction and must be copied for later use. Host history
is excluded from guest snapshots and state digests. --trace=serial_history
records service actions and observation context; --trace=serial records
guest serial events.
