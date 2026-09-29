# Qualification tests

Runner regression tests are grouped by owner: `gdb/`, `qemu/`, and `shared/`.
Fixtures are in
[`../fixtures`](../fixtures/), and scripting examples are in
[`../gdb/examples`](../gdb/examples/). GDB build tests remain in the
[general tools suite](../../tests/README.md).

From the core checkout's `emu/` directory:

```sh
../.venv/bin/python -m unittest discover -s tools/qualification/tests -t .
../.venv/bin/python -m unittest tools.qualification.tests.gdb.test_gdb_execution
../.venv/bin/python -m unittest tools.qualification.tests.qemu.test_qemu_gdb_blockers
make test-tools
make test
```

The blocker tests also invoke the runner with isolated Python `-I`.
Runtime integration tests require built binaries and local sockets.
`make test-tools` includes this suite alongside the general tools, Siemens,
OBEX, Web UI, and CEMU trace suites. The [transport probes](../README.md#transport-probes)
and other explicit qualification gates remain outside unittest discovery.
