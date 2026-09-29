# Qualification runners

Run these modules from pmb7850/emu/. General utilities such as
gdb_build.py, benchmark.py, bundled_firmware.py, diff_drcov.py, and
info_menu_capture.py remain directly under tools/. The UI codec is
tools.ui_protocol.

| Package | Responsibility |
|---|---|
| cemu | Native debugger transport and shutdown |
| qemu | Snapshot continuity, standalone GDB blocker reproduction, external GDB transport |
| gdb | Register, disassembly, execution, memory, stack, scripting, handset, and managed GDB gates |
| shared | Boot matrix, C55 convenience runner, UI observer, transport, QMP, and host helpers |

JSON fixtures live in fixtures/; native GDB scripts live in gdb/examples/.
GDB register/RSP scenarios require QEMU functional helpers. The QEMU
blocker reproducer is a standalone standard-library script.

## Runner examples

~~~sh
../.venv/bin/python -m tools.qualification.gdb.gdb_execution \
  --gdb ../gdb/build/c166/gdb/gdb \
  --qemu ../qemu/build/release/qemu-system-c166 \
  --output shots/gdb-execution
../.venv/bin/python -m tools.qualification.qemu.qemu_snapshots \
  --qemu ../qemu/build/release/qemu-system-c166 \
  --devices c55 --output shots/c55-snapshots
../.venv/bin/python -m tools.qualification.shared.run_x55_boot \
  --verify-images --include-alternates
../.venv/bin/python -m tools.qualification.shared.run_c55_boot \
  --startup-smoke --artifacts shots/c55-startup
~~~

Use --help for a runner's bounds, binary overrides, device/gate selection,
comparison mode, and output options. The boot matrix supports strict and
probe comparisons. A failed probe remains a failure in its report and exit
status. Snapshot continuity does not establish CEMU/QEMU boot parity.
See the [host guide](../../README.md) for Make targets and engine limits.

## Transport probes

Build the host and generated C55 fixture from pmb7850/ first. Build the
QEMU and GDB release binaries before using the two QEMU-based probes:

~~~sh
make -C emu all build/fixtures/generated-c55.bin
make -C emu qemu-release gdb
cd emu
../.venv/bin/python -m tools.qualification.cemu.transport_probe \
  --output shots/debugger-transports
../.venv/bin/python -m tools.qualification.qemu.transport_probe \
  --output shots/debugger-transports \
  --qemu ../qemu/build/release/qemu-system-c166 --gdb ../gdb/build/c166/gdb/gdb
../.venv/bin/python -m tools.qualification.gdb.transport_probe \
  --output shots/debugger-transports \
  --qemu ../qemu/build/release/qemu-system-c166 --gdb ../gdb/build/c166/gdb/gdb
~~~

These probes require Linux PTYs and local sockets. Each accepts --emu and
--fixture overrides. They cover CEMU and QEMU UI/PTY behavior, managed GDB
launch, and shutdown. Reports retain commands, fixture hashes, terminal/MI
logs, UI packets, serial captures, and results. They qualify host transport
behavior, not handset boot fidelity.

See the [unittest suite](tests/README.md) for runner regression tests.
