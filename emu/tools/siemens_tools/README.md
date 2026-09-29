# Siemens Tools

tools.siemens_tools is the CLI for Siemens firmware packages, raw fullflash
images, EEPROM records, and bitmap resources. It accepts user-supplied
inputs and the [bundled fullflashes](../../../firmware/README.md). Use
--help at any command level for complete options.

Firmware unpacking began as a port of the
[siemens-fw-tool](https://github.com/siemens-mobile-hacks/siemens-fw-tool/)
project and includes fixes made here. The remaining commands evolved in
this repository.

## Setup

From the pmb7850/ root:

~~~sh
python3.14 -m venv .venv
.venv/bin/python -m pip install -r emu/requirements.txt
PYTHONPATH=emu .venv/bin/python -m tools.siemens_tools --help
~~~

The command prefix in this guide is
PYTHONPATH=emu .venv/bin/python -m tools.siemens_tools.

## Output safety

- Inspection prints text by default. --json writes machine-readable JSON
  to stdout; diagnostics go to stderr and failures return nonzero.
- Treat source firmware, fullflashes, maps, manifests, and archives as
  immutable. Put generated files under a task-named emu/shots/ directory.
- firmware unpack and fullflash split derive sibling output directories
  if --output is omitted; firmware convert derives a mode-specific suffix.
  Supply an explicit output when input directories should stay untouched.
- Commands with --force reject existing owned outputs unless forced.
  Forced split and catalog builds replace only paths owned by their
  manifests; unrelated files remain. Byte-identical catalog files may
  be reused.
- EEPROM generation/calibration, layout rendering, and bitmap extraction
  have no force gate. Choose a new or deliberately disposable output path.
  Bitmap --output names a directory.
- Catalog build options name catalog roots; other commands may also accept
  catalog.json, recipes, or payloads as shown by their --help. Consumers
  validate versioned manifests, hashes, backing, and paths; rebuild
  dependents after replacing backing data.

## Command index

| Command | Purpose |
|---|---|
| layout MODEL, layout --all | Show flash layouts as text or Markdown |
| firmware info, unpack, convert | Inspect or extract package payloads; convert XBI or validated FFSInit inputs |
| firmware updater info, extract | Inspect transport frames or extract a base-zero C166 updater image and manifest |
| firmware catalog build, info | Build or inspect a versioned package catalog |
| fullflash info, split | Locate raw data and split it at layout boundaries |
| fullflash compose | Apply an ordered composition manifest to produce a complete image |
| fullflash assemble | Build an image from a complete baseline, optional role selections, and personalization |
| fullflash bcore-key derive, recover | Derive or search a BOOTKEY/SKey chain |
| fullflash catalog build, info, tag-patches, reconstruct | Optional catalog and recipe operations for supplied fullflashes |
| eeprom list, extract, diff | Inspect, recover, or compare logical records |
| eeprom generate | Create a deterministic schema-2 identity overlay |
| eeprom status-info | Decode active block 5005 fields |
| eeprom battery inspect, synthesize | Inspect block 67 or replace its calibration endpoints |
| eeprom corpus | Cluster selected records across user-supplied files or trees |
| bitmap | Extract a known descriptor table or scan a fullflash for candidates |

## Layouts and firmware packages

Layouts are case-insensitive and support grouped aliases. Text is the
default; --format markdown renders the same ranges and sizes. --layout-file
selects another layout catalog.

Supported package suffixes are .xbi, .xbz, .xfs, .xci, .xbb, .exci, and
.exbi. Multi-payload executables require --payload INDEX for conversion.
FFSInit recovery fails closed: XFS output needs a byte-exact EXE/XFS match
for model, software version, and embedded ZIP hash. --bin can use a sole
same-model/software reference with an identical canonical file tree and
labels reference-backed wrapper bytes.

firmware updater extract writes mobile-updater.frames.bin,
mobile-updater.c166.bin, and manifest.json. Transport frames are validated
before extraction. The C166 image starts at address zero; FF gaps are
explicit tool policy, and mapped ranges retain transport provenance.

~~~sh
PYTHONPATH=emu .venv/bin/python -m tools.siemens_tools layout c55
PYTHONPATH=emu .venv/bin/python -m tools.siemens_tools firmware info PACKAGE.xbz --json
PYTHONPATH=emu .venv/bin/python -m tools.siemens_tools firmware unpack UPDATE.exe \
  -o emu/shots/package-unpack
~~~

PACKAGE.xbz and UPDATE.exe stand for user-supplied inputs under this
checkout.

## Fullflash

fullflash info scans before placement and retains validated findings even
when placement is unresolved. --layout constrains selection; supply
--flash-address ADDRESS with --file-offset OFFSET for an explicit mapping.
fullflash split writes split-manifest.json with source, native, logical,
and canonical ranges. Erased canonical slices remain ordered manifest
entries without files.

fullflash compose applies an ordered schema-1 manifest and rejects unsafe
paths or uncovered output ranges. fullflash assemble requires a complete
baseline; --non-interactive requires an explicit --baseline and preserves
omitted regions. --region ROLE=erased fills the entire role with FF.
Personalization can set MobSw, flash IDs, BOOTKEY, entry target, or logical
EEPROM. Conflicting writes to an erased owner are rejected. An assembly
writes a binary and a hash-pinned JSON recipe; --recipe replays and verifies
it. Generated identity and named EEPROM profiles are emulator inputs,
not physical calibration or handset-safe EEPROM images.

~~~sh
mkdir -p emu/shots
PYTHONPATH=emu .venv/bin/python -m tools.siemens_tools fullflash info \
  firmware/C55/c55sw249111-556677-d9258d0fe59a.bin --json
PYTHONPATH=emu .venv/bin/python -m tools.siemens_tools fullflash split \
  firmware/C55/c55sw249111-556677-d9258d0fe59a.bin \
  -o emu/shots/c55-split
PYTHONPATH=emu .venv/bin/python -m tools.siemens_tools fullflash compose \
  COMPOSITION.json -o emu/shots/composed.bin
~~~

COMPOSITION.json is supplied by the user. Assembly requires suitable
prepared catalogs; inspect fullflash assemble --help
for selection and replay options. The firmware package and fullflash
catalog commands remain available for supplied inputs, but the bundled
images do not imply any catalog or assembled-output qualification.

## EEPROM

Commands auto-detect the EEPROM region in a complete image. Use
--eeprom-base and --region-linear-base only for partial or shifted inputs.
eeprom generate creates exact schema-2 unlocked identity records;
--identity-only omits optional battery block 67. Battery synthesis changes
calibration endpoints and needs measured ADC points for physical use.

~~~sh
PYTHONPATH=emu .venv/bin/python -m tools.siemens_tools eeprom list \
  firmware/C55/c55sw249111-556677-d9258d0fe59a.bin --json
PYTHONPATH=emu .venv/bin/python -m tools.siemens_tools eeprom generate \
  --imei 11223344556677 --fsn 1234ABCD --identity-only \
  --output emu/shots/identity.json
~~~

Use eeprom extract to recover raw, scrambled, or decrypted records;
eeprom diff compares inventories or selected blocks. eeprom status-info
decodes the active block 5005; eeprom battery inspect decodes block 67.
eeprom corpus clusters exact selected records and profiles from
user-supplied inputs without changing them.

## Bitmap

bitmap requires either --table-address ADDRESS for a known descriptor
table or --scan-flash to find candidates. It supports raw type 0x01,
ARGB4444 type 0x07, compressed type 0x81, and encoding overrides listed
in bitmap --help. Table mode writes a manifest, index, images, and
previews; --compose combines 16-byte records using their x/y trailers.
Scan mode writes scan-manifest.json; --carve-scan adds images and a gallery.

~~~sh
PYTHONPATH=emu .venv/bin/python -m tools.siemens_tools bitmap \
  firmware/C55/c55sw249111-556677-d9258d0fe59a.bin \
  --scan-flash --scan-types 0x01,0x81 --scan-min-pixels 500 \
  --output emu/shots/bitmap-scan
~~~

## Tests

From the pmb7850/ root:

~~~sh
PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=emu .venv/bin/python -m unittest discover \
  -s emu/tools/siemens_tools/tests -t emu
~~~
