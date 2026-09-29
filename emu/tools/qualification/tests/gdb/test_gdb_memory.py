"""Watchpoint qualification must reject wrong stops and missing RSP coverage."""
import json
from pathlib import Path
import tempfile
import time
import unittest

from tools.qualification.gdb.gdb_memory import FIXTURES, REASONS, committed_accesses, packets, stop
from tools.qualification.tests.gdb import test_gdb_execution


class MemoryGateTests(unittest.TestCase):
    def test_conditional_trace_keeps_duplicate_reads_and_excludes_inspection(self):
        read = ('c166_access pc=0x002002 count=2 addr=0x00f13c '
                'size=2 write=0 value=0xa55b\n')
        write = ('c166_access pc=0x002002 count=2 addr=0x00f13c '
                 'size=2 write=1 value=0xa55a\n')
        log = ('c166_test_mmio_read offset=0x6 value=0xa55b size=2\n' +
               read + 'c166_restore pc=0x002002 count=2 entry=1\n' + read + write +
               'c166_test_mmio_read offset=0x6 value=0xa55a size=2\n' +
               read.replace('pc=0x002002', 'pc=0x00200a'))
        self.assertEqual(committed_accesses(log, 0x2002),
                         [[2, 0xf13c, 2, 0, 0xa55b],
                          [2, 0xf13c, 2, 0, 0xa55b],
                          [2, 0xf13c, 2, 1, 0xa55a]])

    def test_watch_stop_checks_reason_pc_and_identity(self):
        for command, (reason, field) in REASONS.items():
            event = (f'*stopped,reason="{reason}",{field}={{number="7"}},'
                     'frame={addr="0x2004"}')
            for expected_number, pc, accepted in [('7', 0x2004, True),
                                                   ('8', 0x2004, False),
                                                   ('7', 0x2000, False)]:
                client = test_gdb_execution.ExecutionGateTests().client([event])
                if accepted:
                    stop(client, time.monotonic()+1, command, expected_number, pc)
                else:
                    with self.assertRaises(AssertionError):
                        stop(client, time.monotonic()+1, command, expected_number, pc)
            client = test_gdb_execution.ExecutionGateTests().client([
                '*stopped,reason="signal-received",signal-name="SIGTRAP",'
                'frame={addr="0x2004"}'])
            with self.assertRaises(AssertionError):
                stop(client, time.monotonic()+1, command, '7', 0x2004)

    def test_packet_coverage_cannot_pass_without_hardware_watches(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            with self.assertRaises(AssertionError):
                packets(out)
            wire = ''.join(f'${case}{kind},fc01,1#00'
                           for kind in (2, 3, 4) for case in ('Z', 'z'))
            (out/'test-rsp.log').write_text('w  '+wire.encode().hex()+'\n')
            packets(out)
            self.assertTrue((out/'watchpoint-packets.json').is_file())

    def test_replay_fixtures_cover_each_stack_position(self):
        fixture = json.loads(FIXTURES.read_text())
        cases = {case['name']:case for case in fixture['cases']}
        self.assertEqual(len(cases), len(fixture['cases']))
        for name, addresses in {'calls':{0xfbfc, 0xfbfe},
                                'rets':{0xfbfc, 0xfbfe},
                                'reti':{0xfbfa, 0xfbfc, 0xfbfe}}.items():
            self.assertTrue(addresses <= {w[1] for w in cases[name]['watches']})
        for case in cases.values():
            self.assertIn('M166', case['manual'])
            self.assertEqual(len(bytes.fromhex(case['code'])) % 2, 0)
            self.assertTrue(case['watches'])
            self.assertIn('setup_memory', case)


if __name__ == '__main__':
    unittest.main()
