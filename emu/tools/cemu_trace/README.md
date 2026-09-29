# CEMU Trace Parquet

Instrumented CEMU runs write a Hive-partitioned Parquet dataset directly.
The [EMU host](../../README.md) owns capture, output paths, and the reader
commands. No intermediate binary or conversion step is needed.

## Build and capture

From the pmb7850/ root, build the host and its pinned Carquet dependency:

~~~sh
git submodule update --init emu/vendor/carquet
make -C emu deps
make -C emu
cd emu
./bin/emu run --trace list
./bin/emu run ../firmware/C55/c55sw249111-556677-d9258d0fe59a.bin \
  --trace=exec,mem,sfr --label boot-trace --limit 8m
~~~

--trace list prints all selector groups and exact event names without an
image. Bare --trace captures all events; comma-separated selectors limit
capture volume without changing the dataset format. For example:

~~~sh
./bin/emu run ../firmware/C55/c55sw249111-556677-d9258d0fe59a.bin \
  --trace=xbus,keypad,lcd --trace-from-icount 2900000 \
  --label focused --limit 8m
~~~

--trace-from-pc ADDR is another deferred activation option. Tracing is off
by default; ordinary CEMU runs do not link the trace writer. The host
selects its instrumented profile when capture is requested.

Each capture writes shots/LABEL/trace/trace.parquet/ with manifest.json and
kind=EVENT/part-NNNNN.parquet partitions. Only observed kinds receive files.
An empty trace has a manifest and remains queryable. The writer uses
ZSTD compression, 65,536-row groups, and a bounded worker queue.

Capture first writes a PID-scoped partial dataset. Successful close finalizes
all Parquet footers and the manifest before atomically installing the dataset.
A failed close reports an error and removes the partial data; a partial
directory is not a completed trace.

## Schema and ordering

Every partition has these common columns:

| Column | Type | Meaning |
|---|---|---|
| seq | BIGINT | Global emission order, starting at zero |
| icount | BIGINT | Guest tick count |
| pc | INTEGER | 24-bit guest PC |
| addr | nullable INTEGER | Access address |
| size | nullable INTEGER | Access width in bytes |
| value | nullable BIGINT | Read, write, or event value |
| detail | VARCHAR | Event-specific diagnostic text |
| kind | VARCHAR | Hive partition name |

Structured info values become typed columns such as info_access_str and
info_selected_bool. Fields that can be null or change type also get
info_NAME_present and info_NAME_type columns. The manifest records each
partition's schema, row count, and sequence range. Sort by seq for
chronological side effects: events at one icount can have different seq values.

## Reader commands and API

From emu/, use the repository virtual environment for the pinned DuckDB:

~~~sh
TRACE=shots/boot-trace/trace/trace.parquet
../.venv/bin/python -m tools.cemu_trace info "$TRACE"
../.venv/bin/python -m tools.cemu_trace verify "$TRACE"
../.venv/bin/python -m tools.cemu_trace sql "$TRACE" \
  'SELECT kind, count(*) AS events FROM trace GROUP BY kind ORDER BY events DESC'
../.venv/bin/python -m tools.cemu_trace xbus "$TRACE" transactions
../.venv/bin/python -m tools.cemu_trace eeprom "$TRACE"
~~~

info prints the manifest. verify checks the finalized v1 format, exact
partition files and schemas, row counts, row-group sizes, and a unique,
gap-free global seq. sql and xbus print tab-separated rows. Add --align
to pad columns for terminal reading; query results are spooled before
alignment so the query executes once.

The public Python helper creates a union-by-name, Hive-partitioned trace
view plus four XBUS views. It accepts a dataset path or manifest.json,
and an optional existing DuckDB connection:

~~~python
from tools.cemu_trace import open_parquet

con = open_parquet("shots/boot-trace/trace/trace.parquet")
rows = con.execute(
    "SELECT seq, icount, kind FROM trace ORDER BY seq LIMIT 20"
).fetchall()
con.close()
~~~

For a focused side-effect query:

~~~sql
SELECT seq, icount, printf('0x%06x', pc) AS pc, kind,
       printf('0x%06x', addr) AS addr, size, value
FROM trace
WHERE icount BETWEEN 2934000 AND 2941000
ORDER BY seq;
~~~

## XBUS views

open_parquet() creates these views even for an empty trace:

| View | Contents |
|---|---|
| xbus_accesses | Ordered, duplicate-suppressed XBUS and in-window memory accesses |
| xbus_registers | Per-window/address counts, bounds, PCs, and distinct values |
| xbus_transactions | Doorbells, preceding mailbox state, completions, and sequence bounds |
| xbus_effects | Completion phases, status polls/clears, result reads, and scheduler-latch traffic |

~~~sh
../.venv/bin/python -m tools.cemu_trace xbus "$TRACE" accesses
../.venv/bin/python -m tools.cemu_trace xbus "$TRACE" registers
../.venv/bin/python -m tools.cemu_trace xbus "$TRACE" transactions
../.venv/bin/python -m tools.cemu_trace xbus "$TRACE" effects
../.venv/bin/python -m tools.cemu_trace sql "$TRACE" \
  'SELECT * FROM xbus_effects ORDER BY seq LIMIT 200'
~~~

## EEPROM reports

The read-only eeprom command summarizes logical block activity from
eeprom_access and eeprom_map events:

~~~sh
../.venv/bin/python -m tools.cemu_trace eeprom "$TRACE"
../.venv/bin/python -m tools.cemu_trace eeprom --all "$TRACE"
../.venv/bin/python -m tools.cemu_trace eeprom --json "$TRACE"
~~~

Multiple trace paths produce separate reports in argument order; histories
are never merged. The default report includes blocks with observed payload
activity. --all also shows mapped but unobserved and metadata-only blocks.
Absence from a bounded capture is not evidence that a block is unnecessary.

The report separates resolved and shadowed payload activity from descriptor
metadata. Counts and byte totals retain repeated and overlapping transfers;
coverage counts unique byte ranges. Writes before a block is allocated
remain in the unattributed journal. A later mapping can also label those
spans post-hoc; unclaimed spans remain unresolved. Ordering uses
block-attributed payload reads and committed program/erase writes, not
metadata reads. JSON uses the cemu-eeprom-report-v1 envelope.

A trace with neither EEPROM event kind is a valid empty report. The reader
rejects malformed EEPROM partitions and an eeprom_access partition without
the eeprom_map history required for a complete report.

## Tests

~~~sh
make -C emu test-tools
~~~

This includes the CEMU Trace reader suite and the Parquet-enabled native
trace fixture.
