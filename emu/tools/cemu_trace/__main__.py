"""Metadata, verification, and SQL for direct CEMU Parquet traces."""
from __future__ import annotations

import argparse
import json
import pickle
from pathlib import Path
import tempfile
from urllib.parse import quote

from .eeprom import create_report, format_report
from .parquet import open_parquet

FORMAT = "cemu-trace-parquet-v1"
SCHEMA = 1
ROW_GROUP_ROWS = 65_536


class TraceError(RuntimeError):
    pass


def _load_manifest(path: Path) -> tuple[Path, dict]:
    root = path.parent if path.name == "manifest.json" else path
    manifest_path = root / "manifest.json"
    try:
        manifest = json.loads(manifest_path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise TraceError(f"cannot read {manifest_path}: {exc}") from exc
    expected = {
        "format": FORMAT, "schema": SCHEMA, "finalized": True,
        "compression": "zstd", "row_group_rows": ROW_GROUP_ROWS,
    }
    for key, value in expected.items():
        if manifest.get(key) != value:
            raise TraceError(f"invalid {key}: {manifest.get(key)!r}, expected {value!r}")
    events = manifest.get("events")
    partitions = manifest.get("partitions")
    if not isinstance(events, int) or events < 0:
        raise TraceError(f"invalid events: {events!r}")
    if not isinstance(partitions, list):
        raise TraceError("invalid partitions list")
    seen_kinds: set[str] = set()
    seen_paths: set[str] = set()
    for partition in partitions:
        if not isinstance(partition, dict):
            raise TraceError("invalid partition entry")
        kind = partition.get("kind")
        expected_path = (
            f"kind={quote(kind, safe='-_.~')}/part-00000.parquet"
            if isinstance(kind, str) and kind else None
        )
        if kind in seen_kinds or partition.get("path") in seen_paths:
            raise TraceError(f"duplicate partition: {kind!r}")
        if partition.get("path") != expected_path:
            raise TraceError(f"invalid partition path for {kind!r}: {partition.get('path')!r}")
        rows = partition.get("rows")
        first = partition.get("first_seq")
        last = partition.get("last_seq")
        columns = partition.get("columns")
        if not isinstance(rows, int) or rows <= 0:
            raise TraceError(f"invalid row count for {kind!r}: {rows!r}")
        if not isinstance(first, int) or not isinstance(last, int) or first > last:
            raise TraceError(f"invalid sequence bounds for {kind!r}: {first!r}..{last!r}")
        if not isinstance(columns, list) or not all(
            isinstance(column, dict) and set(column) == {"name", "type"}
            and isinstance(column["name"], str) and isinstance(column["type"], str)
            for column in columns
        ):
            raise TraceError(f"invalid columns for {kind!r}")
        seen_kinds.add(kind)
        seen_paths.add(expected_path)
    if sum(partition["rows"] for partition in partitions) != events:
        raise TraceError("manifest partition rows do not equal events")
    return root, manifest


def _info(path: Path) -> int:
    root, manifest = _load_manifest(path)
    summary = dict(manifest)
    summary["path"] = str(root)
    print(json.dumps(summary, sort_keys=True))
    return 0


def _verify(path: Path) -> int:
    root, manifest = _load_manifest(path)
    partitions = manifest["partitions"]
    expected_files = {"manifest.json", *(partition["path"] for partition in partitions)}
    actual_files = {str(item.relative_to(root)) for item in root.rglob("*") if item.is_file()}
    if actual_files != expected_files:
        missing = sorted(expected_files - actual_files)
        extra = sorted(actual_files - expected_files)
        raise TraceError(f"dataset files differ: missing={missing}, extra={extra}")

    connection = open_parquet(root)
    try:
        for partition in partitions:
            file_path = root / partition["path"]
            schema = connection.execute(
                "SELECT name, duckdb_type FROM parquet_schema(?) "
                "WHERE column_id > 0 ORDER BY column_id", [str(file_path)]
            ).fetchall()
            expected_schema = [
                (column["name"], column["type"]) for column in partition["columns"]
            ]
            if schema != expected_schema:
                raise TraceError(f"schema mismatch for {partition['kind']!r}")

            groups = connection.execute(
                "SELECT row_group_id, row_group_num_rows, min(compression) "
                "FROM parquet_metadata(?) GROUP BY ALL ORDER BY row_group_id",
                [str(file_path)],
            ).fetchall()
            expected_groups = []
            remaining = partition["rows"]
            group_id = 0
            while remaining:
                rows = min(remaining, ROW_GROUP_ROWS)
                expected_groups.append((group_id, rows, "ZSTD"))
                remaining -= rows
                group_id += 1
            if groups != expected_groups:
                raise TraceError(f"row groups mismatch for {partition['kind']!r}: {groups!r}")

            rows, distinct, first, last, regressions = connection.execute(
                "SELECT count(*), count(DISTINCT seq), min(seq), max(seq), "
                "count(*) FILTER (WHERE previous_seq IS NOT NULL AND seq <= previous_seq) "
                "FROM (SELECT seq, lag(seq) OVER () AS previous_seq FROM read_parquet(?))",
                [str(file_path)],
            ).fetchone()
            expected = (partition["rows"], partition["rows"],
                        partition["first_seq"], partition["last_seq"], 0)
            if (rows, distinct, first, last, regressions) != expected:
                raise TraceError(f"partition sequence mismatch for {partition['kind']!r}")

        count, distinct, first, last = connection.execute(
            "SELECT count(*), count(DISTINCT seq), min(seq), max(seq) FROM trace"
        ).fetchone()
    finally:
        connection.close()
    events = manifest["events"]
    expected_bounds = (None, None) if not events else (0, events - 1)
    if (count, distinct, first, last) != (events, events, *expected_bounds):
        raise TraceError(
            f"global sequence mismatch: rows={count} distinct={distinct} range={first}..{last}"
        )
    print(f"OK: {count} events in {len(partitions)} partitions")
    return 0


def _print_query(path: Path, query: str, align: bool = False) -> int:
    connection = open_parquet(path)
    try:
        result = connection.execute(query)
        if result.description:
            header = [column[0] for column in result.description]
            if not align:
                print("\t".join(header))
                while rows := result.fetchmany(1024):
                    for row in rows:
                        print("\t".join(
                            "NULL" if value is None else str(value)
                            for value in row
                        ))
                return 0

            widths = [len(value) for value in header]
            spool = tempfile.TemporaryFile()
            try:
                while rows := result.fetchmany(1024):
                    for row in rows:
                        cells = [
                            "NULL" if value is None else str(value)
                            for value in row
                        ]
                        for index, value in enumerate(cells):
                            widths[index] = max(widths[index], len(value))
                        pickle.dump(cells, spool, protocol=pickle.HIGHEST_PROTOCOL)

                print("\t".join(
                    value.rjust(widths[index])
                    for index, value in enumerate(header)
                ))
                spool.seek(0)
                while True:
                    try:
                        cells = pickle.load(spool)
                    except EOFError:
                        break
                    print("\t".join(
                        value.rjust(widths[index])
                        for index, value in enumerate(cells)
                    ))
            finally:
                spool.close()
    finally:
        connection.close()
    return 0


def _xbus(path: Path, view: str, align: bool = False) -> int:
    ordering = {
        "accesses": "seq",
        "registers": "window_name, addr",
        "transactions": "doorbell_seq",
        "effects": "seq",
    }
    return _print_query(
        path, f"SELECT * FROM xbus_{view} ORDER BY {ordering[view]}", align
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m tools.cemu_trace")
    commands = parser.add_subparsers(dest="command", required=True)
    info = commands.add_parser("info", help="print capture metadata")
    info.add_argument("trace", type=Path)
    verify = commands.add_parser("verify", help="validate partitions, counts, and ordering")
    verify.add_argument("trace", type=Path)
    sql = commands.add_parser("sql", help="run SQL against a trace.parquet directory")
    sql.add_argument(
        "--align", action="store_true",
        help="right-align each column with left space padding",
    )
    sql.add_argument("parquet", type=Path)
    sql.add_argument("query")
    xbus = commands.add_parser("xbus", help="query built-in XBUS trace views")
    xbus.add_argument(
        "--align", action="store_true",
        help="right-align each column with left space padding",
    )
    xbus.add_argument("trace", type=Path)
    xbus.add_argument(
        "view",
        choices=("accesses", "registers", "transactions", "effects"),
    )
    eeprom = commands.add_parser(
        "eeprom", help="report logical EEPROM block activity",
    )
    eeprom.add_argument(
        "--all", action="store_true",
        help="include mapped, metadata-only, and otherwise unobserved blocks",
    )
    eeprom.add_argument(
        "--json", action="store_true", help="print the stable JSON report envelope",
    )
    eeprom.add_argument("trace", nargs="+", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "info":
            return _info(args.trace)
        if args.command == "verify":
            return _verify(args.trace)
        if args.command == "sql":
            return _print_query(args.parquet, args.query, args.align)
        if args.command == "xbus":
            return _xbus(args.trace, args.view, args.align)
        report = create_report(args.trace, show_all=args.all)
        if args.json:
            print(json.dumps(report, indent=2))
        else:
            print(format_report(report))
        return 0
    except (OSError, RuntimeError, TraceError) as exc:
        parser.exit(1, f"error: {exc}\n")


if __name__ == "__main__":
    raise SystemExit(main())
