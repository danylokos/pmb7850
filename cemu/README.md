# CEMU — PMB7850 / E-GOLD+ V3 core

CEMU is the C166S CPU and PMB7850 SoC engine used by the [EMU host](../emu/README.md).
The host owns the stable CLI, image preparation, transports, and artifacts. CEMU
builds two static libraries: build/lib/libcemu.a for ordinary runs and
build/lib/libcemu_inst.a for instrumented diagnostics.

## Build and run

From the pmb7850/ root, after the [host setup](../emu/README.md#build-this-checkout):

~~~sh
make -C cemu
make -C cemu test
make -C emu
emu/bin/emu run firmware/C55/c55sw249111-556677-d9258d0fe59a.bin \
  --limit 8m --monitor --summary
~~~

make -C emu test runs host and tool tests. emu/bin/cemu and emu/bin/cemu_inst
are internal compatibility runners. Use emu/bin/emu run for scripts;
emu/bin/emu run --help lists current options. The host selects the
instrumented profile when a requested feature needs it.

An ordinary run stops at its tick limit. --monitor can stop on spin,
waiting_io, waiting_dpram, or a complete EXIT: serial record; --summary
enables monitoring and prints the run summary. --show-writes adds write
counts. --benchmark-json writes one object with invocation-local ticks,
executed instructions, elapsed time, status, and state digest; it cannot
be combined with the debugger or summary. --batch-idle skips only proven
event-free IDLE spans. Debugger step uses ordinary stepping.

## Engine layout

| Area | Responsibility |
|---|---|
| src/cpu/ | C166S decode, execution, flags, addressing, interrupts, disassembly |
| src/soc/ | Memory decode, interrupt/PEC service, peripheral composition, device configuration |
| src/soc/peripherals/ | Flash, serial, timers, ports, keypad, LCD, audio, SIM, other modeled blocks |
| src/diagnostics/ | Snapshots, state digests, monitor, debugger, synthetic controls, observers |
| tests/ | CPU, SoC, peripheral, and diagnostic unit tests |

CPU semantics belong in src/cpu/; common SoC behavior belongs in src/soc/
or a peripheral; board differences belong in device_config_t under
src/soc/devices/. Peripherals expose register and byte-range hooks, interrupt
nodes, and names through peripheral_t. The memory controller resolves flash
aliases and calls flash models with chip-relative addresses.

## Inputs and modeled identity

The host validates Siemens image metadata before boot and detects the device
from its model tag. --device NAME overrides board wiring without changing the
image's software/language IDs or resolved primary flash engine. Missing,
conflicting, erased, or unsupported metadata is an error. The current device
list is in emu/bin/emu run --help.

Fresh boots use deterministic unlocked identity (FSN 1234ABCD, IMEI
11223344556677) and seed resolvable EEPROM identity records. This is emulator
policy, not recovered factory data. --fsn XXXXXXXX supplies a known FSN;
--imei supplies a 14-digit AM29 customer SecSi mirror and requires --fsn.
For a generated identity profile:

~~~sh
cd emu
mkdir -p shots
PYTHONPATH=. ../.venv/bin/python -m tools.siemens_tools eeprom generate \
  --fsn A35F2F28 --imei 35335000894548 --output shots/identity.json
./bin/emu run FULLFLASH.bin --eeprom-overlay shots/identity.json --limit 100m
~~~

Schema-2 overlays replace existing directory-addressed records
67/76/5008/5009/5077. --fsn, --eeprom-overlay, and --from-snapshot
are mutually exclusive. Snapshots already own flash identity and overlay
state. See [Siemens Tools](../emu/tools/siemens_tools/README.md#eeprom) for
the bundle format and generation options.

--sim attaches a deterministic card profile. --synth list lists diagnostic
behaviors; active nondefault controls are labeled SYNTHETIC. The default-off
--synth gsm is a C55 SW24-specific responder/publication demo. It does not
implement an OAK DSP, BCCH decoding, or location update. Repeat --sim and
--synth gsm after snapshot restore because attachments are invocation policy.
--patch list lists audited, model/software-scoped firmware patches. Patches
change guest-visible flash and persist in snapshots; start from the source
image to remove them.

## Trace and artifacts

Tracing is off by default. List selectors without starting a capture:

~~~sh
emu/bin/emu run --trace list
emu/bin/emu run FULLFLASH.bin --trace=serial,tdma --label my-run --limit 8m
cd emu
PYTHONPATH=. ../.venv/bin/python -m tools.cemu_trace info shots/my-run/trace/trace.parquet
PYTHONPATH=. ../.venv/bin/python -m tools.cemu_trace verify shots/my-run/trace/trace.parquet
~~~

Bare --trace captures all events. --trace-from-icount N and
--trace-from-pc ADDR defer activation. Events have a monotonic sequence
number to order side effects at the same icount. See the
[CEMU Trace guide](../emu/tools/cemu_trace/README.md) for schemas and queries.
--drcov writes coverage/cov.drcov independently of trace activation.

Artifact switches take no path. --label NAME chooses shots/NAME/ relative
to the run's working directory; otherwise the host generates a label.

| Switch | Output within shots/LABEL/ |
|---|---|
| --trace[=SELECTORS] | trace/trace.parquet/ |
| --drcov | coverage/cov.drcov |
| --snapshot | snapshot/ |
| --dump-flash | flash.bin, the physical main array at stop |
| --lcd-frames | Displayed LCD PNGs and lcd/frames.gif |
| --lcd-ddram-frames | Raw controller-memory PNGs in lcd-ddram/ |

--snapshot-at N requires --snapshot. --snapshot-full-bins adds CPU-visible
static-analysis bins, which can reflect a flash controller's current read
mode. Use --dump-flash for a bootable main-array image; it excludes separate
factory UID, OTP, and SecSi identity. To materialize a snapshot without
advancing guest time:

~~~sh
cd emu
./bin/emu run --from-snapshot shots/RUN/snapshot --limit 0 \
  --dump-flash --label recovered
~~~

LCD frames show displayed output; raw DDRAM frames expose received memory
even when the display is off or a repaint is unfinished.

## Debugger and serial transport

The CEMU debugger accepts -c 'COMMANDS', --script FILE, or -i. Use help
in the REPL for complete syntax. Common commands:

~~~text
break|b ADDR                delete|d ADDR
watch|w ADDR[..END]         rwatch ADDR[..END]   awatch ADDR[..END]
wlog|wl ADDR[..END] [K]     cont|c [MAX]        step|si [N]
regs|p  peripherals|periph bt  x ADDR [N]      sfr ADDR [VALUE]
set ADDR VALUE [size]       setr REG VALUE     disasm|di ADDR [N]
monitor [on|off]            checkpoint|cp      restore
pin P3.N|P6.N|P7.N|P8.N 0|1 edge CCN rise|fall
key NAME down|up            serial-rx HEX      twi [REG [VALUE]]
irq IRQ45|IRQ64..IRQ79|XP2 [ILVL]
eeprom list|show|stats|watch|log|watches|unwatch ...
trace PATH [SELECTORS]      quit|q
~~~

Bare watch stops after an instruction changes a watched byte. rwatch and
awatch stop on reads or any access, excluding instruction fetches. Explicit
watch ADDR[..END] K keeps access-filter behavior (r, w, rw, sfr, sfrr,
sfrw). wlog logs without stopping. Inspection and set/setr/sfr pokes issue
no bus transactions. eeprom commands inspect logical records without flash
bus reads; watches follow block remaps. irq is a diagnostic injection,
not a modeled hardware source.

~~~sh
cd emu
mkdir -p shots
./bin/emu run FULLFLASH.bin -c 'break 0x94a152; cont; regs; bt'
./bin/emu run --from-snapshot shots/RUN/snapshot -c 'regs; step 10; bt'
./bin/emu run FULLFLASH.bin --serial-pty shots/obex.pty
~~~

--serial-pty PATH bridges raw ASC0 bytes to a new PTY path, which must not
already exist. It does not model electrical bits, modem-control lines, or
parity/framing faults. Debugger serial-rx HEX queues up to 240 bytes through
timed ASC0 reception. Debugger cont has its own optional step cap and
currently bypasses CLI --limit, deferred trace triggers, and custom monitor
thresholds; use cont MAX or step N to bound it. See the
[OBEX client guide](../emu/tools/obex/README.md) for a serial client.

## Validation

~~~sh
make -C cemu test
make -C emu test
~~~

cemu/tests/ checks core and peripheral behavior; emu/tests/ checks host
and adapter contracts. Tests for a selected image and run bound do not
establish every handset or firmware release.
