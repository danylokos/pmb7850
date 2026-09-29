"""Execution-gate regressions: stop attribution and bounded failure matter."""
import io
import json
import queue
import time
import unittest
from unittest.mock import Mock

from tools.qualification.gdb.gdb_execution import FIXTURES, MI, mi_fields


class ExecutionGateTests(unittest.TestCase):
    def client(self, lines):
        m = MI.__new__(MI)
        m.records = []
        m.lines = queue.Queue()
        m.log = io.StringIO()
        m.proc = Mock()
        for line in lines:
            m.lines.put(line)
        return m

    def test_mi_nested_breakpoint_list(self):
        fields = mi_fields('BreakpointTable={nr_rows="2",body=['
                           'bkpt={number="1",addr="0x2000"},'
                           'bkpt={number="2",addr="0x2002"}]}')
        self.assertEqual([b['number'] for _, b in fields['BreakpointTable']['body']],
                         ['1', '2'])
        self.assertEqual(mi_fields('args=[],signal-name="SIGINT"')['args'], [])

    def test_stop_requires_cause_pc_and_identity(self):
        event = '*stopped,reason="breakpoint-hit",bkptno="2",frame={addr="0x2000"}'
        self.client([event]).stop(time.monotonic()+1, 'breakpoint-hit', 0x2000, '2')
        for reason, pc, bp in [('end-stepping-range', 0x2000, '2'),
                               ('breakpoint-hit', 0x2002, '2'),
                               ('breakpoint-hit', 0x2000, '1')]:
            with self.assertRaises(AssertionError):
                self.client([event]).stop(time.monotonic()+1, reason, pc, bp)

    def test_timeout_never_becomes_a_stop(self):
        m = self.client(['*running,thread-id="all"', '1^running'])
        with self.assertRaises(TimeoutError):
            m.stop(time.monotonic()+0.01, 'end-stepping-range')
        self.assertEqual(len(m.records), 2)

    def test_queued_stop_cannot_rescue_an_expired_operation(self):
        m = self.client(['*stopped,reason="end-stepping-range"'])
        with self.assertRaises(TimeoutError):
            m.stop(time.monotonic(), 'end-stepping-range')

    def test_event_consumed_once_and_out_of_order_retained(self):
        m = self.client(['*running,thread-id="all"', '1^running',
                         '*stopped,reason="end-stepping-range"'])
        self.assertEqual(m.wait(lambda x: x == '1^running', time.monotonic()+1),
                         '1^running')
        m.event('running', time.monotonic()+1)
        m.stop(time.monotonic()+1, 'end-stepping-range')
        with self.assertRaises(TimeoutError):
            m.stop(time.monotonic(), 'end-stepping-range')

    def test_fixture_coverage_and_instruction_counts(self):
        f = json.loads(FIXTURES.read_text())
        cases = {c['name']:c for c in f['cases']}
        self.assertEqual(len(cases), len(f['cases']))
        for name in ('calla', 'calli', 'callr', 'calls', 'pcall'):
            self.assertIn(name, cases)
            self.assertTrue(cases[name]['steps'][0]['memory'])
        for name in ('atomic', 'extr', 'exts', 'extp', 'extsr', 'extpr'):
            forms = ('short',) if name in ('atomic','extr') else ('immediate','register')
            for form in forms:
                for count in range(1,5):
                    case = cases[f'{name}-{form}-{count}']
                    self.assertEqual(case['steps'][0]['registers']['ext_count'],count)
                    self.assertEqual(case['steps'][-1]['registers']['ext_count'],0)
        for c in cases.values():
            self.assertIn('M166', c['manual'])
            self.assertEqual(len(bytes.fromhex(c['code'])) % 2, 0)
            for i, step in enumerate(c['steps'],1):
                self.assertEqual(step['command'],'stepi')
                self.assertEqual(step['registers']['guest_icount'],i)
                self.assertIn('pc',step['registers'])


if __name__ == '__main__':
    unittest.main()
