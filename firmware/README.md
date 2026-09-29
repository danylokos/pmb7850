# Bundled fullflash inputs

This directory holds 36 generated fullflash images for twelve PMB7850
handset configurations. Treat the Git-tracked files as immutable inputs.

## Manifest

[manifest.json](manifest.json) records each image's relative path, SHA-256,
size, model, software/language/T9 versions, flash topology, generated
identity, and bounded-startup result. Its devices entries choose one
default per model; alternates are nested under each device. A default
is a convenient selection, not a qualification ranking.

The images use generated FSN 1234ABCD and IMEI 11223344556677.
--fsn 1234ABCD selects matching modeled flash identity while preserving
their EEPROM records. The startup checks need no overlay, patch, or
synthetic behavior; --sim changes the simulated handset environment.

All 36 images were created with [Siemens Tools](../emu/tools/siemens_tools/README.md)
fullflash assemble and the same static SKey 12345678 with FSN 1234ABCD.
This derives BOOTKEY (BKEY) e830b0e07fd1e91e0e9e8eb7c2affc8d; the
assembler writes its digest f822ec654a39627a56f4cb9a6becdb56 into
each layout's BCORE field. These are generated emulator keys, not
recovered handset secrets.

## Verify and run

From the pmb7850/ root:

~~~sh
PYTHONPATH=emu .venv/bin/python -m tools.qualification.shared.run_x55_boot \
  --verify-images --include-alternates
emu/bin/emu run firmware/C55/c55sw249111-556677-d9258d0fe59a.bin \
  --fsn 1234ABCD --limit 100k
PYTHONPATH=emu .venv/bin/python -m tools.qualification.shared.run_x55_boot \
  --startup-smoke --include-alternates \
  --artifacts emu/shots/standalone-startup
~~~

--verify-images checks paths, sizes, and hashes without QEMU or GDB.
The [EMU host guide](../emu/README.md#build-this-checkout) covers builds
and engine options. --startup-smoke needs built CEMU and QEMU binaries.

## Validation limits

The manifest records 100,000-tick CEMU smoke runs and QEMU startup probes
for each image, without SIM, overlay, or patch. QEMU stops asynchronously
after its UI protocol reports the target, so its endpoint can exceed
100,000 ticks. The pmb7850-bounded-startup-report records hashes,
commands, endpoints, and results for both engines.

These checks establish image loading and early execution only: full boot,
usable UI, SIM, audio, and engine parity remain unqualified. The matrix's
20,000,000-tick bounds are probe budgets; no-sim is its default gate,
with sim and supervised also available as probes.

The files combine Siemens firmware content with generated identity and
emulator-calibrated EEPROM data. Firmware provenance and distribution
permissions are separate from the emulator source license.
