from __future__ import annotations

from dataclasses import dataclass

from .storage import Block


_GREEN = "\x1b[32m"
_RED = "\x1b[31m"
_DIM = "\x1b[2m"
_RESET = "\x1b[0m"


@dataclass(frozen=True)
class BlockDiff:
    """One block that is present in both sources but whose bytes differ."""
    block_id: int
    len_a: int
    len_b: int
    first_diff: int | None  # None when only directory metadata differs
    version_a: int | None
    version_b: int | None
    memory_class_a: int | None
    memory_class_b: int | None


@dataclass(frozen=True)
class DiffResult:
    only_in_a: list[int]
    only_in_b: list[int]
    differ: list[BlockDiff]
    equal: list[int]

    @property
    def equal_count(self) -> int:
        return len(self.equal)

    @property
    def common(self) -> int:
        return self.equal_count + len(self.differ)


def _first_diff_offset(a: bytes, b: bytes) -> int:
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return i
    return n  # differ only in length; first divergence is at the shorter end


def diff_sources(a: dict[int, Block], b: dict[int, Block]) -> DiffResult:
    """Compare two {id: Block} inventories on raw stored bytes."""
    ids_a, ids_b = set(a), set(b)
    only_in_a = sorted(ids_a - ids_b)
    only_in_b = sorted(ids_b - ids_a)
    differ: list[BlockDiff] = []
    equal: list[int] = []
    for bid in sorted(ids_a & ids_b):
        pa, pb = a[bid].payload, b[bid].payload
        metadata_equal = (
            a[bid].version == b[bid].version
            and a[bid].memory_class == b[bid].memory_class
        )
        if pa == pb and metadata_equal:
            equal.append(bid)
        else:
            first_diff = None if pa == pb else _first_diff_offset(pa, pb)
            differ.append(BlockDiff(
                bid, len(pa), len(pb), first_diff,
                a[bid].version, b[bid].version,
                a[bid].memory_class, b[bid].memory_class,
            ))
    return DiffResult(only_in_a, only_in_b, differ, equal)


def render_block_bytediff(a: bytes, b: bytes, color: bool = True) -> str:
    """Render two blocks' bytes side by side, 16/row, matching green / differing red.

    Length differences pad the shorter side; a missing byte shows as ``--``. The
    offset and ASCII gutters follow the ``hex_dump`` style. When ``color`` is
    False no ANSI escapes are emitted (for pipes / --json / tests).
    """
    def paint(text: str, code: str) -> str:
        return f"{code}{text}{_RESET}" if color else text

    def col(data: bytes, other: bytes, off: int) -> tuple[str, str]:
        cells, chars = [], []
        for i in range(off, off + 16):
            if i < len(data):
                same = i < len(other) and data[i] == other[i]
                cells.append(paint(f"{data[i]:02x}", _GREEN if same else _RED))
                ch = chr(data[i]) if 0x20 <= data[i] <= 0x7E else "."
                chars.append(paint(ch, _GREEN if same else _RED))
            else:
                cells.append(paint("--", _DIM))
                chars.append(" ")
        return " ".join(cells), "".join(chars)

    lines = []
    total = max(len(a), len(b))
    for off in range(0, total, 16):
        ah, ac = col(a, b, off)
        bh, bc = col(b, a, off)
        lines.append(f"{off:04x}  {ah}  {ac}   |   {bh}  {bc}")
    return "\n".join(lines)




# --------------------------------------------------------------------------- #
# CLI: list / extract / diff / generate subcommands
# --------------------------------------------------------------------------- #
