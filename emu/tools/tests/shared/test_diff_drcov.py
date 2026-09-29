"""CLI regression test for emu/tools/diff_drcov.py."""

from __future__ import annotations

import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[4]
TOOL = REPO_ROOT / 'emu' / 'tools' / 'diff_drcov.py'


MODULES = [
    (0, 0x800000, 0x801000, 'flash_native'),
    (1, 0x010000, 0x011000, 'seg1_lm'),
    (2, 0x000000, 0x001000, 'seg0_low'),
]


def write_drcov(path: Path, blocks: list[tuple[int, int]]) -> None:
    header = [
        'DRCOV VERSION: 2',
        'DRCOV FLAVOR: drcov',
        f'Module Table: version 2, count {len(MODULES)}',
        'Columns: id, base, end, entry, checksum, timestamp, path',
    ]
    for module_id, base, end, name in MODULES:
        header.append(f'{module_id}, {base:#x}, {end:#x}, 0x0, 0x0, 0x0, {name}')

    body = bytearray()
    for addr, size in sorted(blocks):
        for module_id, base, end, _ in MODULES:
            if base <= addr < end:
                body += struct.pack('<IHH', addr - base, size, module_id)
                break
        else:
            raise AssertionError(f'no module for block {addr:#x}')

    path.write_bytes(("\n".join(header) + f"\nBB Table: {len(blocks)} bbs\n").encode('utf-8') + body)


class DiffDrcovTests(unittest.TestCase):
    def test_json_and_text(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            a = root / 'a.drcov'
            b = root / 'b.drcov'
            write_drcov(a, [
                (0x800100, 2),
                (0x800102, 2),
                (0x010000, 2),
            ])
            write_drcov(b, [
                (0x800102, 2),
                (0x800104, 2),
                (0x000100, 2),
            ])

            proc = subprocess.run(
                [sys.executable, str(TOOL), str(a), str(b), '--json',
                 '--show', 'summary,a-b,b-a,xor,intersect'],
                check=True,
                capture_output=True,
                text=True,
            )
            report = json.loads(proc.stdout)
            self.assertEqual(report['counts'], {
                'shared': 1,
                'a_minus_b': 2,
                'b_minus_a': 2,
                'xor': 4,
                'union': 5,
            })
            self.assertEqual(report['sections']['a-b']['modules'], [
                {'module': 'flash@0x800000', 'blocks': 1},
                {'module': 'seg1_lm@0x010000', 'blocks': 1},
            ])
            self.assertEqual(report['sections']['b-a']['modules'], [
                {'module': 'flash@0x800000', 'blocks': 1},
                {'module': 'seg0_low@0x000000', 'blocks': 1},
            ])
            self.assertEqual(report['sections']['a-b']['spans'], [
                {'module': 'seg1_lm@0x010000', 'start': '0x010000',
                 'end': '0x010002', 'bytes': 2, 'blocks': 1},
                {'module': 'flash@0x800000', 'start': '0x800100',
                 'end': '0x800102', 'bytes': 2, 'blocks': 1},
            ])

            output = subprocess.run(
                [sys.executable, str(TOOL), str(a), str(b), '--limit', '1',
                 '--show', 'a-b,b-a,xor'],
                check=True,
                capture_output=True,
                text=True,
            ).stdout
            self.assertIn('A-B: 2 block(s), 2 span(s)', output)
            self.assertIn('B-A: 2 block(s), 2 span(s)', output)
            self.assertIn('A^B: 4 block(s), 4 span(s)', output)
            self.assertIn('seg1_lm@0x010000 [0x010000, 0x010002)', output)


if __name__ == '__main__':
    unittest.main()
