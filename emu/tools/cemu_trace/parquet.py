"""DuckDB view helper for native CEMU Trace Parquet datasets."""
from __future__ import annotations

import json
from pathlib import Path

from .xbus import create_xbus_views


def _sql_string(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def open_parquet(path: str | Path, connection=None):
    try:
        import duckdb
    except ImportError as exc:
        raise RuntimeError(
            "duckdb is not installed; run `.venv/bin/pip install -r emu/requirements.txt`"
        ) from exc

    root = Path(path).resolve()
    if root.is_file() and root.name == "manifest.json":
        root = root.parent
    manifest = root / "manifest.json"
    if not manifest.is_file():
        raise FileNotFoundError(f"Parquet manifest not found: {manifest}")
    try:
        metadata = json.loads(manifest.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"cannot read Parquet manifest {manifest}: {exc}") from exc
    if (metadata.get("format"), metadata.get("schema"), metadata.get("finalized")) != (
        "cemu-trace-parquet-v1", 1, True
    ):
        raise RuntimeError(f"unsupported or incomplete Parquet trace: {manifest}")

    con = connection or duckdb.connect()
    files = list(root.glob("kind=*/part-*.parquet"))
    if files:
        pattern = str(root / "kind=*" / "part-*.parquet")
        con.execute(
            "CREATE OR REPLACE VIEW trace AS SELECT * FROM read_parquet("
            f"{_sql_string(pattern)}, union_by_name=true, hive_partitioning=true)"
        )
    else:
        con.execute(
            "CREATE OR REPLACE VIEW trace AS SELECT "
            'CAST(NULL AS BIGINT) AS "seq", CAST(NULL AS BIGINT) AS "icount", '
            'CAST(NULL AS INTEGER) AS "pc", CAST(NULL AS INTEGER) AS "addr", '
            'CAST(NULL AS INTEGER) AS "size", CAST(NULL AS BIGINT) AS "value", '
            'CAST(NULL AS VARCHAR) AS "detail", '
            'CAST(NULL AS VARCHAR) AS "kind" WHERE false'
        )
    create_xbus_views(con)
    return con
