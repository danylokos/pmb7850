# General tool tests

This suite tests general utilities directly under `tools/`:
GDB building and binary smoke checks in `gdb/`, and benchmark, bundled firmware,
DRcov comparison, and info-menu capture in `shared/`.

Qualification runners, transport probes, fixtures, and their tests live in
[`tools/qualification`](../qualification/README.md). Siemens Tools, Web UI,
OBEX, and CEMU Trace have dedicated suites.

From the core repository root:

```sh
make -C emu test-tools
make -C emu test
.venv/bin/python -m unittest discover -s emu/tools/tests -t emu
(cd emu && ../.venv/bin/python -m unittest tools.tests.gdb.test_gdb_build)
```

`make test-tools` discovers this suite and each dedicated package suite once.
Explicit qualification commands are outside unittest discovery.
