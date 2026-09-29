"""Streaming logical EEPROM activity reports for CEMU Parquet traces."""
from __future__ import annotations

from dataclasses import dataclass, field
import json
from pathlib import Path
from typing import Any, Iterable

from .parquet import open_parquet


REPORT_FORMAT = "cemu-eeprom-report-v1"
OPERATIONS = ("read", "program", "erase")

_COMMON_COLUMNS = {
    "seq": "BIGINT", "icount": "BIGINT", "pc": "INTEGER",
    "addr": "INTEGER", "size": "INTEGER", "value": "BIGINT",
    "detail": "VARCHAR",
}
_ACCESS_COLUMNS = {
    **_COMMON_COLUMNS,
    "info_access_str": "VARCHAR",
    "info_area_str": "VARCHAR",
    "info_area_offset_i64": "BIGINT",
    "info_attribution_str": "VARCHAR",
    "info_block_id_present": "BOOLEAN",
    "info_block_id_type": "VARCHAR",
    "info_block_id_i64": "BIGINT",
    "info_block_offset_present": "BOOLEAN",
    "info_block_offset_type": "VARCHAR",
    "info_block_offset_i64": "BIGINT",
    "info_chip_index_i64": "BIGINT",
    "info_chip_name_str": "VARCHAR",
    "info_chip_offset_i64": "BIGINT",
    "info_mapping_generation_i64": "BIGINT",
    "info_model_str": "VARCHAR",
    "info_mutation_offset_present": "BOOLEAN",
    "info_mutation_offset_type": "VARCHAR",
    "info_mutation_offset_i64": "BIGINT",
    "info_mutation_size_present": "BOOLEAN",
    "info_mutation_size_type": "VARCHAR",
    "info_mutation_size_i64": "BIGINT",
    "info_tick_i64": "BIGINT",
}
_MAP_COLUMNS = {
    **_COMMON_COLUMNS,
    "info_block_id_i64": "BIGINT",
    "info_change_str": "VARCHAR",
    "info_chip_index_i64": "BIGINT",
    "info_chip_name_str": "VARCHAR",
    "info_current_active_present": "BOOLEAN",
    "info_current_active_type": "VARCHAR",
    "info_current_active_bool": "BOOLEAN",
    "info_current_descriptor_offset_present": "BOOLEAN",
    "info_current_descriptor_offset_type": "VARCHAR",
    "info_current_descriptor_offset_i64": "BIGINT",
    "info_current_length_present": "BOOLEAN",
    "info_current_length_type": "VARCHAR",
    "info_current_length_i64": "BIGINT",
    "info_current_linear_present": "BOOLEAN",
    "info_current_linear_type": "VARCHAR",
    "info_current_linear_i64": "BIGINT",
    "info_current_payload_offset_present": "BOOLEAN",
    "info_current_payload_offset_type": "VARCHAR",
    "info_current_payload_offset_i64": "BIGINT",
    "info_current_version_present": "BOOLEAN",
    "info_current_version_type": "VARCHAR",
    "info_current_version_i64": "BIGINT",
    "info_mapping_generation_i64": "BIGINT",
    "info_model_str": "VARCHAR",
    "info_previous_active_present": "BOOLEAN",
    "info_previous_active_type": "VARCHAR",
    "info_previous_active_bool": "BOOLEAN",
    "info_previous_descriptor_offset_present": "BOOLEAN",
    "info_previous_descriptor_offset_type": "VARCHAR",
    "info_previous_descriptor_offset_i64": "BIGINT",
    "info_previous_length_present": "BOOLEAN",
    "info_previous_length_type": "VARCHAR",
    "info_previous_length_i64": "BIGINT",
    "info_previous_linear_present": "BOOLEAN",
    "info_previous_linear_type": "VARCHAR",
    "info_previous_linear_i64": "BIGINT",
    "info_previous_payload_offset_present": "BOOLEAN",
    "info_previous_payload_offset_type": "VARCHAR",
    "info_previous_payload_offset_i64": "BIGINT",
    "info_previous_version_present": "BOOLEAN",
    "info_previous_version_type": "VARCHAR",
    "info_previous_version_i64": "BIGINT",
    "info_tick_i64": "BIGINT",
}


class EepromReportError(RuntimeError):
    """The trace cannot produce a complete EEPROM report."""


def _merge_ranges(ranges: Iterable[tuple[int, int]]) -> list[list[int]]:
    merged: list[list[int]] = []
    for start, end in sorted(ranges):
        if start >= end:
            continue
        if merged and start <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], end)
        else:
            merged.append([start, end])
    return merged


def _add_range(ranges: list[tuple[int, int]], start: int, end: int) -> None:
    """Maintain a sorted interval union while rows stream past."""
    combined: list[tuple[int, int]] = []
    inserted = False
    for old_start, old_end in ranges:
        if old_end < start:
            combined.append((old_start, old_end))
        elif end < old_start:
            if not inserted:
                combined.append((start, end))
                inserted = True
            combined.append((old_start, old_end))
        else:
            start = min(start, old_start)
            end = max(end, old_end)
    if not inserted:
        combined.append((start, end))
    ranges[:] = combined


@dataclass
class _Activity:
    counts: dict[str, int] = field(
        default_factory=lambda: {operation: 0 for operation in OPERATIONS}
    )
    byte_counts: dict[str, int] = field(
        default_factory=lambda: {operation: 0 for operation in OPERATIONS}
    )
    ranges: list[tuple[int, int]] = field(default_factory=list)
    read_pcs: set[int] = field(default_factory=set)
    write_pcs: set[int] = field(default_factory=set)
    first_by_operation: dict[str, int | None] = field(
        default_factory=lambda: {operation: None for operation in OPERATIONS}
    )
    seen_by_operation: dict[str, set[int]] = field(
        default_factory=lambda: {operation: set() for operation in OPERATIONS}
    )
    first: tuple[int, int, int] | None = None
    last: tuple[int, int, int] | None = None

    def add(self, event: dict[str, Any], start: int, size: int) -> None:
        operation = event["access"]
        if event["seq"] not in self.seen_by_operation[operation]:
            self.seen_by_operation[operation].add(event["seq"])
            self.counts[operation] += 1
        self.byte_counts[operation] += size
        _add_range(self.ranges, start, start + size)
        (self.read_pcs if operation == "read" else self.write_pcs).add(event["pc"])
        timing = (event["seq"], event["icount"], event["tick"])
        first_operation = self.first_by_operation[operation]
        if first_operation is None or event["seq"] < first_operation:
            self.first_by_operation[operation] = event["seq"]
        if self.first is None or timing[0] < self.first[0]:
            self.first = timing
        if self.last is None or timing[0] > self.last[0]:
            self.last = timing

    @property
    def total_count(self) -> int:
        return sum(self.counts.values())

    def render(self, coverage_space: str) -> dict[str, Any]:
        return {
            "counts": dict(self.counts),
            "transferred_bytes": {
                **self.byte_counts, "total": sum(self.byte_counts.values()),
            },
            "coverage_space": coverage_space,
            "coverage": _merge_ranges(self.ranges),
        }


@dataclass
class _Block:
    chip: tuple[int, str, str]
    block_id: int
    resolved: _Activity = field(default_factory=_Activity)
    shadowed: _Activity = field(default_factory=_Activity)
    metadata: _Activity = field(default_factory=_Activity)
    post_descriptor: _Activity = field(default_factory=_Activity)
    post_payload: _Activity = field(default_factory=_Activity)
    generations: set[int] = field(default_factory=set)
    lengths: set[int] = field(default_factory=set)
    changes: list[dict[str, Any]] = field(default_factory=list)
    final_mapping: dict[str, Any] | None = None

    def activities(self) -> tuple[_Activity, ...]:
        return (
            self.resolved, self.shadowed, self.metadata,
            self.post_descriptor, self.post_payload,
        )

    def has_payload_activity(self) -> bool:
        return any(activity.total_count for activity in (
            self.resolved, self.shadowed, self.post_payload,
        ))


@dataclass
class _JournalSpan:
    event: dict[str, Any]
    ranges: list[tuple[int, int]]


class _Analyzer:
    def __init__(self) -> None:
        self.blocks: dict[tuple[tuple[int, str, str], int], _Block] = {}
        self.non_block: dict[tuple[int, str, str], dict[str, _Activity]] = {}
        self.pending: dict[tuple[int, str, str], list[_JournalSpan]] = {}
        self.chips_by_index: dict[int, tuple[int, str, str]] = {}
        self.last_seq = -1

    def _block(self, chip: tuple[int, str, str], block_id: int) -> _Block:
        return self.blocks.setdefault((chip, block_id), _Block(chip, block_id))

    def _non_block(self, chip: tuple[int, str, str], name: str) -> _Activity:
        groups = self.non_block.setdefault(chip, {})
        return groups.setdefault(name, _Activity())

    def _chip(self, event: dict[str, Any]) -> tuple[int, str, str]:
        chip = (event["chip_index"], event["chip_name"], event["model"])
        if chip[0] < 0 or not chip[1] or not chip[2]:
            raise EepromReportError(f"invalid chip identity at seq {event['seq']}")
        previous = self.chips_by_index.setdefault(chip[0], chip)
        if previous != chip:
            raise EepromReportError(
                f"inconsistent chip identity for index {chip[0]} at seq {event['seq']}"
            )
        return chip

    def consume(self, event: dict[str, Any]) -> None:
        if event["seq"] <= self.last_seq:
            raise EepromReportError("EEPROM events are not in strictly increasing sequence order")
        self.last_seq = event["seq"]
        if event["kind"] == "eeprom_access":
            self._access(event)
        else:
            self._mapping(event)

    def _access(self, event: dict[str, Any]) -> None:
        chip = self._chip(event)
        operation = event["access"]
        if operation not in OPERATIONS or event["size"] is None or event["size"] <= 0:
            raise EepromReportError(f"invalid EEPROM access at seq {event['seq']}")
        attribution = event["attribution"]
        block_id = event["block_id"]
        if attribution in ("resolved", "shadowed"):
            if block_id is None or event["block_offset"] is None:
                raise EepromReportError(f"payload access lacks a block at seq {event['seq']}")
            block = self._block(chip, block_id)
            block.generations.add(event["generation"])
            activity = block.resolved if attribution == "resolved" else block.shadowed
            activity.add(event, event["block_offset"], event["size"])
        elif attribution == "metadata":
            if block_id is None:
                self._non_block(chip, "metadata").add(
                    event, event["chip_offset"], event["size"]
                )
            else:
                block = self._block(chip, block_id)
                block.generations.add(event["generation"])
                block.metadata.add(event, event["area_offset"], event["size"])
        elif attribution in ("unattributed", "unattributed-journal"):
            if block_id is not None:
                raise EepromReportError(f"unattributed access has a block at seq {event['seq']}")
            self._non_block(chip, attribution).add(
                event, event["chip_offset"], event["size"]
            )
            if attribution == "unattributed-journal":
                if operation == "read":
                    raise EepromReportError(f"journal read at seq {event['seq']}")
                start = event["chip_offset"]
                self.pending.setdefault(chip, []).append(
                    _JournalSpan(event, [(start, start + event["size"])])
                )
        else:
            raise EepromReportError(
                f"unknown EEPROM attribution {attribution!r} at seq {event['seq']}"
            )

    @staticmethod
    def _mapping_state(event: dict[str, Any], prefix: str) -> dict[str, Any] | None:
        length = event[f"{prefix}_length"]
        values = {
            "active": event[f"{prefix}_active"],
            "descriptor_offset": event[f"{prefix}_descriptor_offset"],
            "length": length,
            "linear": event[f"{prefix}_linear"],
            "payload_offset": event[f"{prefix}_payload_offset"],
            "version": event[f"{prefix}_version"],
        }
        if all(value is None for value in values.values()):
            return None
        if any(value is None for value in values.values()) or length < 0:
            raise EepromReportError(
                f"partial {prefix} mapping at seq {event['seq']}"
            )
        return values

    def _mapping(self, event: dict[str, Any]) -> None:
        chip = self._chip(event)
        block_id = event["block_id"]
        change = event["change"]
        if block_id is None or block_id < 0 or change not in (
            "initial", "added", "remapped", "removed"
        ):
            raise EepromReportError(f"invalid mapping at seq {event['seq']}")
        previous = self._mapping_state(event, "previous")
        current = self._mapping_state(event, "current")
        if ((change in ("initial", "added") and current is None)
                or (change == "remapped" and (previous is None or current is None))
                or (change == "removed" and (previous is None or current is not None))):
            raise EepromReportError(f"inconsistent {change} mapping at seq {event['seq']}")

        block = self._block(chip, block_id)
        if change in ("initial", "added") and previous is not None:
            raise EepromReportError(f"inconsistent {change} mapping at seq {event['seq']}")
        if change == "initial" and block.changes:
            raise EepromReportError(f"duplicate initial mapping at seq {event['seq']}")
        if change == "added" and block.changes and block.final_mapping is not None:
            raise EepromReportError(f"added existing mapping at seq {event['seq']}")
        if change in ("remapped", "removed") and (
            not block.changes or block.final_mapping != previous
        ):
            raise EepromReportError(f"mapping history diverges at seq {event['seq']}")
        block.generations.add(event["generation"])
        for state in (previous, current):
            if state is not None:
                block.lengths.add(state["length"])
        change_record = {
            "seq": event["seq"], "icount": event["icount"],
            "tick": event["tick"], "pc": event["pc"],
            "generation": event["generation"], "change": change,
            "previous": previous, "current": current,
        }
        block.changes.append(change_record)
        block.final_mapping = current
        if change in ("added", "remapped"):
            self._attribute_journal(chip, block, current)

    def _attribute_journal(
        self, chip: tuple[int, str, str], block: _Block, mapping: dict[str, Any]
    ) -> None:
        targets = (
            (mapping["descriptor_offset"], mapping["descriptor_offset"] + 12,
             block.post_descriptor),
            (mapping["payload_offset"], mapping["payload_offset"] + mapping["length"],
             block.post_payload),
        )
        for span in self.pending.get(chip, []):
            for target_start, target_end, activity in targets:
                remaining: list[tuple[int, int]] = []
                for start, end in span.ranges:
                    hit_start = max(start, target_start)
                    hit_end = min(end, target_end)
                    if hit_start < hit_end:
                        activity.add(
                            span.event, hit_start - target_start, hit_end - hit_start
                        )
                        block.generations.add(span.event["generation"])
                        if start < hit_start:
                            remaining.append((start, hit_start))
                        if hit_end < end:
                            remaining.append((hit_end, end))
                    else:
                        remaining.append((start, end))
                span.ranges = remaining

    def finish(self, path: str, show_all: bool) -> dict[str, Any]:
        for chip, spans in self.pending.items():
            unresolved = self._non_block(chip, "unresolved-journal")
            for span in spans:
                for start, end in span.ranges:
                    unresolved.add(span.event, start, end - start)

        rendered_blocks = []
        for (_, _), block in sorted(
            self.blocks.items(), key=lambda item: (*item[0][0], item[0][1])
        ):
            if not show_all and not block.has_payload_activity():
                continue
            rendered_blocks.append(self._render_block(block))
        rendered_non_block = []
        for chip, groups in sorted(self.non_block.items()):
            rendered_groups = {
                name: activity.render("chip-offset")
                for name, activity in sorted(groups.items())
                if activity.total_count
            }
            if rendered_groups:
                rendered_non_block.append({
                    "chip": _render_chip(chip), "activity": rendered_groups,
                })
        return {
            "path": path,
            "blocks": rendered_blocks,
            "non_block_activity": rendered_non_block,
        }

    @staticmethod
    def _render_block(block: _Block) -> dict[str, Any]:
        activities = block.activities()
        first = min(
            (activity.first for activity in activities if activity.first is not None),
            default=None,
        )
        last = max(
            (activity.last for activity in activities if activity.last is not None),
            default=None,
        )
        read_pcs = sorted(set().union(*(activity.read_pcs for activity in activities)))
        write_pcs = sorted(set().union(*(activity.write_pcs for activity in activities)))
        payload_activities = (block.resolved, block.shadowed, block.post_payload)
        reads = [
            activity.first_by_operation["read"] for activity in payload_activities
            if activity.first_by_operation["read"] is not None
        ]
        writes = [
            sequence for activity in payload_activities
            for operation in ("program", "erase")
            if (sequence := activity.first_by_operation[operation]) is not None
        ]
        if reads and writes:
            ordering = "read-before-write" if min(reads) < min(writes) else "write-before-read"
        elif reads:
            ordering = "read-only"
        elif writes:
            ordering = "write-only"
        else:
            ordering = "unobserved"
        return {
            "chip": _render_chip(block.chip),
            "block_id": block.block_id,
            "mappings": {
                "generations": sorted(block.generations),
                "observed_lengths": sorted(block.lengths),
                "final": block.final_mapping,
                "changes": block.changes,
            },
            "activity": {
                "resolved": block.resolved.render("block-payload-offset"),
                "shadowed": block.shadowed.render("block-payload-offset"),
                "metadata": block.metadata.render("descriptor-offset"),
                "post_hoc": {
                    "descriptor": block.post_descriptor.render("descriptor-offset"),
                    "payload": block.post_payload.render("block-payload-offset"),
                },
            },
            "pcs": {"read": read_pcs, "write": write_pcs},
            "timing": {"first": _render_timing(first), "last": _render_timing(last)},
            "ordering": ordering,
        }


def _render_chip(chip: tuple[int, str, str]) -> dict[str, Any]:
    return {"index": chip[0], "name": chip[1], "model": chip[2]}


def _render_timing(timing: tuple[int, int, int] | None) -> dict[str, int] | None:
    if timing is None:
        return None
    return {"seq": timing[0], "icount": timing[1], "tick": timing[2]}


def _trace_root_and_partitions(path: Path) -> tuple[Path, dict[str, dict[str, Any]]]:
    root = path.parent if path.name == "manifest.json" else path
    manifest_path = root / "manifest.json"
    try:
        manifest = json.loads(manifest_path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise EepromReportError(f"cannot read {manifest_path}: {exc}") from exc
    if (manifest.get("format"), manifest.get("schema"), manifest.get("finalized")) != (
        "cemu-trace-parquet-v1", 1, True,
    ):
        raise EepromReportError(f"unsupported or incomplete Parquet trace: {manifest_path}")
    raw = manifest.get("partitions")
    if not isinstance(raw, list):
        raise EepromReportError(f"invalid partitions in {manifest_path}")
    partitions = {
        item.get("kind"): item for item in raw
        if isinstance(item, dict) and isinstance(item.get("kind"), str)
    }
    if len(partitions) != len(raw):
        raise EepromReportError(f"invalid or duplicate partitions in {manifest_path}")
    return root, partitions


def _validate_schema(kind: str, partition: dict[str, Any]) -> None:
    expected = _ACCESS_COLUMNS if kind == "eeprom_access" else _MAP_COLUMNS
    columns = partition.get("columns")
    if not isinstance(columns, list) or any(
        not isinstance(column, dict) or set(column) != {"name", "type"}
        for column in columns
    ):
        raise EepromReportError(f"malformed {kind} schema")
    actual = {column["name"]: column["type"] for column in columns}
    if actual != expected or len(actual) != len(columns):
        raise EepromReportError(f"malformed {kind} schema")


_QUERY = """
SELECT seq, icount, pc, size, kind,
       info_access_str AS access, info_area_str AS area,
       info_area_offset_i64 AS area_offset,
       info_attribution_str AS attribution,
       info_block_id_i64 AS block_id,
       info_block_offset_i64 AS block_offset,
       info_chip_index_i64 AS chip_index,
       info_chip_name_str AS chip_name,
       info_chip_offset_i64 AS chip_offset,
       info_mapping_generation_i64 AS generation,
       info_model_str AS model, info_tick_i64 AS tick,
       info_change_str AS change,
       info_current_active_bool AS current_active,
       info_current_descriptor_offset_i64 AS current_descriptor_offset,
       info_current_length_i64 AS current_length,
       info_current_linear_i64 AS current_linear,
       info_current_payload_offset_i64 AS current_payload_offset,
       info_current_version_i64 AS current_version,
       info_previous_active_bool AS previous_active,
       info_previous_descriptor_offset_i64 AS previous_descriptor_offset,
       info_previous_length_i64 AS previous_length,
       info_previous_linear_i64 AS previous_linear,
       info_previous_payload_offset_i64 AS previous_payload_offset,
       info_previous_version_i64 AS previous_version
FROM trace
WHERE kind IN ('eeprom_access', 'eeprom_map')
ORDER BY seq
"""

_MAP_ONLY_QUERY = """
SELECT seq, icount, pc, size, kind,
       NULL::VARCHAR AS access, NULL::VARCHAR AS area,
       NULL::BIGINT AS area_offset, NULL::VARCHAR AS attribution,
       info_block_id_i64 AS block_id, NULL::BIGINT AS block_offset,
       info_chip_index_i64 AS chip_index,
       info_chip_name_str AS chip_name, NULL::BIGINT AS chip_offset,
       info_mapping_generation_i64 AS generation,
       info_model_str AS model, info_tick_i64 AS tick,
       info_change_str AS change,
       info_current_active_bool AS current_active,
       info_current_descriptor_offset_i64 AS current_descriptor_offset,
       info_current_length_i64 AS current_length,
       info_current_linear_i64 AS current_linear,
       info_current_payload_offset_i64 AS current_payload_offset,
       info_current_version_i64 AS current_version,
       info_previous_active_bool AS previous_active,
       info_previous_descriptor_offset_i64 AS previous_descriptor_offset,
       info_previous_length_i64 AS previous_length,
       info_previous_linear_i64 AS previous_linear,
       info_previous_payload_offset_i64 AS previous_payload_offset,
       info_previous_version_i64 AS previous_version
FROM trace
WHERE kind = 'eeprom_map'
ORDER BY seq
"""


def analyze_trace(path: str | Path, *, show_all: bool = False) -> dict[str, Any]:
    """Analyze one trace without combining its provenance with another trace."""
    input_path = Path(path)
    root, partitions = _trace_root_and_partitions(input_path)
    access = partitions.get("eeprom_access")
    mapping = partitions.get("eeprom_map")
    if access is None and mapping is None:
        return {"path": str(input_path), "blocks": [], "non_block_activity": []}
    if access is not None and mapping is None:
        raise EepromReportError(
            f"{input_path}: eeprom_access requires eeprom_map mapping history"
        )
    if access is not None:
        _validate_schema("eeprom_access", access)
    assert mapping is not None
    _validate_schema("eeprom_map", mapping)

    analyzer = _Analyzer()
    connection = open_parquet(root)
    try:
        result = connection.execute(_QUERY if access is not None else _MAP_ONLY_QUERY)
        names = [description[0] for description in result.description]
        while rows := result.fetchmany(4096):
            for row in rows:
                analyzer.consume(dict(zip(names, row)))
    finally:
        connection.close()
    return analyzer.finish(str(input_path), show_all)


def create_report(paths: Iterable[str | Path], *, show_all: bool = False) -> dict[str, Any]:
    return {
        "format": REPORT_FORMAT,
        "traces": [analyze_trace(path, show_all=show_all) for path in paths],
    }


def format_report(report: dict[str, Any]) -> str:
    """Render a deterministic, compact human report."""
    lines: list[str] = []
    for trace_index, trace in enumerate(report["traces"]):
        if trace_index:
            lines.append("")
        lines.append(f"Trace: {trace['path']}")
        if not trace["blocks"]:
            lines.append("  (no observed EEPROM blocks)")
        for block in trace["blocks"]:
            chip = block["chip"]
            lines.append(
                f"  Chip 0x{chip['index']:x} {chip['name']} ({chip['model']}), "
                f"block 0x{block['block_id']:04x} ({block['block_id']})"
            )
            mappings = block["mappings"]
            final = mappings["final"]
            final_text = "removed" if final is None else (
                f"active={str(final['active']).lower()} "
                f"descriptor=0x{final['descriptor_offset']:x} "
                f"payload=0x{final['payload_offset']:x} length=0x{final['length']:x} "
                f"linear=0x{final['linear']:06x} version=0x{final['version']:x}"
            )
            lines.append(
                "    mapping: generations=" + _number_list(mappings["generations"])
                + " lengths=" + _hex_list(mappings["observed_lengths"])
                + " final=" + final_text
            )
            changes = ", ".join(
                f"{change['change']}@g{change['generation']}/seq{change['seq']}"
                for change in mappings["changes"]
            ) or "none"
            lines.append(f"    changes: {changes}")
            activity = block["activity"]
            for label, value in (
                ("resolved", activity["resolved"]),
                ("shadowed", activity["shadowed"]),
                ("metadata", activity["metadata"]),
                ("post-hoc descriptor", activity["post_hoc"]["descriptor"]),
                ("post-hoc payload", activity["post_hoc"]["payload"]),
            ):
                lines.append(f"    {label}: {_format_activity(value)}")
            pcs = block["pcs"]
            lines.append(
                f"    PCs: read={_hex_list(pcs['read'])} write={_hex_list(pcs['write'])}"
            )
            timing = block["timing"]
            lines.append(
                f"    timing: first={_format_timing(timing['first'])} "
                f"last={_format_timing(timing['last'])} ordering={block['ordering']}"
            )
        for non_block in trace["non_block_activity"]:
            chip = non_block["chip"]
            lines.append(
                f"  Non-block chip 0x{chip['index']:x} {chip['name']} ({chip['model']})"
            )
            for name, activity in non_block["activity"].items():
                lines.append(f"    {name}: {_format_activity(activity)}")
    return "\n".join(lines)


def _number_list(values: list[int]) -> str:
    return ",".join(str(value) for value in values) if values else "none"


def _hex_list(values: list[int]) -> str:
    return ",".join(f"0x{value:x}" for value in values) if values else "none"


def _format_activity(activity: dict[str, Any]) -> str:
    counts = activity["counts"]
    ranges = activity["coverage"]
    coverage = ",".join(f"[0x{start:x},0x{end:x})" for start, end in ranges) or "none"
    return (
        f"read={counts['read']} program={counts['program']} erase={counts['erase']} "
        f"bytes={activity['transferred_bytes']['total']} coverage={coverage}"
    )


def _format_timing(timing: dict[str, int] | None) -> str:
    if timing is None:
        return "none"
    return f"seq{timing['seq']}/icount{timing['icount']}/tick{timing['tick']}"
