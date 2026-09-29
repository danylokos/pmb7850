#!/usr/bin/env python3
"""Diff two drcov coverage files.

This compares drcov files as sets of unique executed instruction blocks
``(addr, size)``. That makes it useful for localizing *where* two runs executed
something different, but drcov does not preserve execution order, so this tool
cannot identify the first temporal divergence by itself.

Examples:
    python tools/diff_drcov.py shots/a/coverage/cov.drcov shots/b/coverage/cov.drcov
    python tools/diff_drcov.py a.drcov b.drcov --show a-b,b-a,xor --limit 50
    python tools/diff_drcov.py a.drcov b.drcov --json
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Module:
    module_id: int
    base: int
    end: int
    name: str


@dataclass(frozen=True, order=True)
class Block:
    addr: int
    size: int


@dataclass(frozen=True)
class Coverage:
    path: Path
    modules: tuple[Module, ...]
    blocks: frozenset[Block]


@dataclass(frozen=True)
class Span:
    module_base: int
    module_label: str
    start: int
    end: int
    block_count: int

    @property
    def size(self) -> int:
        return self.end - self.start


def module_label(module: Module) -> str:
    if module.base == 0x800000:
        return 'flash@0x800000'
    return f'{module.name}@{module.base:#08x}'


def parse_show(text: str) -> tuple[str, ...]:
    allowed = {'summary', 'a-b', 'b-a', 'xor', 'intersect'}
    items = []
    for raw in text.split(','):
        item = raw.strip().lower()
        if not item:
            continue
        if item not in allowed:
            raise argparse.ArgumentTypeError(f'unknown section: {raw}')
        if item not in items:
            items.append(item)
    if not items:
        raise argparse.ArgumentTypeError('show list must not be empty')
    return tuple(items)


def read_drcov(path: Path) -> Coverage:
    raw = path.read_bytes()
    marker = b'BB Table: '
    idx = raw.find(marker)
    if idx < 0:
        raise ValueError(f'{path}: missing BB Table header')
    nl = raw.find(b'\n', idx)
    if nl < 0:
        raise ValueError(f'{path}: malformed BB Table header')

    header_text = raw[:nl].decode('utf-8')
    lines = header_text.splitlines()
    modules = []
    for line in lines:
        if not line or not line[0].isdigit():
            continue
        parts = [part.strip() for part in line.split(',', 6)]
        if len(parts) != 7:
            raise ValueError(f'{path}: malformed module row: {line!r}')
        modules.append(Module(
            module_id=int(parts[0], 0),
            base=int(parts[1], 0),
            end=int(parts[2], 0),
            name=parts[6],
        ))
    mod_by_id = {module.module_id: module for module in modules}

    count_field = raw[idx + len(marker):nl]
    bb_count = int(count_field.split(b' ', 1)[0])
    body = raw[nl + 1:]
    need = bb_count * 8
    if len(body) < need:
        raise ValueError(f'{path}: truncated BB table ({len(body)} < {need})')

    blocks = set()
    for off in range(0, need, 8):
        offset, size, module_id = struct.unpack_from('<IHH', body, off)
        module = mod_by_id.get(module_id)
        if module is None:
            raise ValueError(f'{path}: BB record references unknown module id {module_id}')
        blocks.add(Block(module.base + offset, size))

    return Coverage(
        path=path,
        modules=tuple(sorted(modules, key=lambda m: m.base, reverse=True)),
        blocks=frozenset(blocks),
    )


def merge_modules(*coverages: Coverage) -> tuple[Module, ...]:
    by_base: dict[int, Module] = {}
    for coverage in coverages:
        for module in coverage.modules:
            prev = by_base.get(module.base)
            if prev is None:
                by_base[module.base] = Module(-1, module.base, module.end, module.name)
            else:
                by_base[module.base] = Module(-1, module.base, max(prev.end, module.end), prev.name)
    return tuple(sorted(by_base.values(), key=lambda m: m.base, reverse=True))


def module_for(addr: int, modules: tuple[Module, ...]) -> Module:
    for module in modules:
        if module.base <= addr < module.end:
            return module
    return Module(-1, 0, 0x1000000, 'full-space')


def counts_by_module(blocks: frozenset[Block] | set[Block], modules: tuple[Module, ...]) -> list[tuple[str, int]]:
    counts: dict[int, int] = {}
    module_bases = {module.base for module in modules}
    for block in blocks:
        module = module_for(block.addr, modules)
        counts[module.base] = counts.get(module.base, 0) + 1
    rows = []
    for module in modules:
        count = counts.get(module.base)
        if count:
            rows.append((module_label(module), count))
    for base in sorted((base for base in counts if base not in module_bases), reverse=True):
        rows.append((f'unknown@{base:#08x}', counts[base]))
    return rows


def blocks_to_spans(blocks: frozenset[Block] | set[Block], modules: tuple[Module, ...]) -> list[Span]:
    spans: list[Span] = []
    for block in sorted(blocks):
        module = module_for(block.addr, modules)
        label = module_label(module)
        start = block.addr
        end = block.addr + block.size
        if spans and spans[-1].module_base == module.base and start <= spans[-1].end:
            prev = spans[-1]
            spans[-1] = Span(
                module_base=prev.module_base,
                module_label=prev.module_label,
                start=prev.start,
                end=max(prev.end, end),
                block_count=prev.block_count + 1,
            )
            continue
        spans.append(Span(module.base, label, start, end, 1))
    return spans


def section_payload(blocks: frozenset[Block] | set[Block], modules: tuple[Module, ...], limit: int) -> dict:
    spans = blocks_to_spans(blocks, modules)
    return {
        'blocks': len(blocks),
        'modules': [
            {'module': label, 'blocks': count}
            for label, count in counts_by_module(blocks, modules)
        ],
        'spans': [
            {
                'module': span.module_label,
                'start': f'{span.start:#08x}',
                'end': f'{span.end:#08x}',
                'bytes': span.size,
                'blocks': span.block_count,
            }
            for span in spans[:limit]
        ],
        'span_count': len(spans),
    }


def compare(a: Coverage, b: Coverage, limit: int, show: tuple[str, ...]) -> dict:
    modules = merge_modules(a, b)
    shared = a.blocks & b.blocks
    a_only = a.blocks - b.blocks
    b_only = b.blocks - a.blocks
    xor = a.blocks ^ b.blocks

    out = {
        'a': {
            'path': str(a.path),
            'blocks': len(a.blocks),
            'modules': [
                {
                    'label': module_label(module),
                    'base': f'{module.base:#08x}',
                    'end': f'{module.end:#08x}',
                    'name': module.name,
                }
                for module in a.modules
            ],
        },
        'b': {
            'path': str(b.path),
            'blocks': len(b.blocks),
            'modules': [
                {
                    'label': module_label(module),
                    'base': f'{module.base:#08x}',
                    'end': f'{module.end:#08x}',
                    'name': module.name,
                }
                for module in b.modules
            ],
        },
        'counts': {
            'shared': len(shared),
            'a_minus_b': len(a_only),
            'b_minus_a': len(b_only),
            'xor': len(xor),
            'union': len(a.blocks | b.blocks),
        },
        'sections': {},
    }

    lookup = {
        'a-b': a_only,
        'b-a': b_only,
        'xor': xor,
        'intersect': shared,
    }
    for section in show:
        if section == 'summary':
            continue
        out['sections'][section] = section_payload(lookup[section], modules, limit)
    return out


def print_text(report: dict, show: tuple[str, ...], limit: int) -> None:
    print(f"A: {report['a']['path']} ({report['a']['blocks']} blocks)")
    print(f"B: {report['b']['path']} ({report['b']['blocks']} blocks)")
    counts = report['counts']
    print(
        'Counts: '
        f"shared={counts['shared']}  A-B={counts['a_minus_b']}  "
        f"B-A={counts['b_minus_a']}  A^B={counts['xor']}  union={counts['union']}"
    )

    if 'summary' in show:
        print('Modules:')
        for side in ('a', 'b'):
            print(f"  {side.upper()} modules:")
            for module in report[side]['modules']:
                print(f"    {module['label']} [{module['base']}, {module['end']})  {module['name']}")

    title = {
        'a-b': 'A-B',
        'b-a': 'B-A',
        'xor': 'A^B',
        'intersect': 'A&B',
    }
    for section in ('a-b', 'b-a', 'xor', 'intersect'):
        if section not in show:
            continue
        payload = report['sections'][section]
        print(f"{title[section]}: {payload['blocks']} block(s), {payload['span_count']} span(s)")
        if payload['modules']:
            print('  By module:')
            for row in payload['modules']:
                print(f"    {row['module']}: {row['blocks']}")
        else:
            print('  By module: none')
        if payload['spans']:
            print(f"  Spans (first {min(limit, len(payload['spans']))}):")
            for span in payload['spans']:
                print(
                    f"    {span['module']} [{span['start']}, {span['end']}) "
                    f"blocks={span['blocks']} bytes={span['bytes']}"
                )
        else:
            print('  Spans: none')


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('a', type=Path, help='first drcov file')
    parser.add_argument('b', type=Path, help='second drcov file')
    parser.add_argument('--show', type=parse_show,
                        default=('summary', 'a-b', 'b-a', 'xor'),
                        help='comma list: summary,a-b,b-a,xor,intersect '
                             '(default: summary,a-b,b-a,xor)')
    parser.add_argument('--limit', type=int, default=20,
                        help='max spans to print per section (default: 20)')
    parser.add_argument('--json', action='store_true', help='emit JSON instead of text')
    args = parser.parse_args(argv)

    if args.limit <= 0:
        parser.error('--limit must be positive')

    try:
        cov_a = read_drcov(args.a)
        cov_b = read_drcov(args.b)
    except (OSError, ValueError) as exc:
        print(f'error: {exc}', file=sys.stderr)
        return 2

    report = compare(cov_a, cov_b, args.limit, args.show)
    if args.json:
        print(json.dumps(report, indent=2, ensure_ascii=False))
    else:
        print_text(report, args.show, args.limit)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
