"""Guard the disassembly gate against silent alignment/byte-listing loss."""
import json
import unittest

from tools.qualification.gdb.gdb_disassembly import FIXTURES, check_listing, fixture_coverage


class DisassemblyGateTests(unittest.TestCase):
    def test_manual_fixture_coverage(self):
        fixtures = json.loads(FIXTURES.read_text())['cases']
        for case in fixtures:
            self.assertIn('M166', case['manual'])
            self.assertEqual(len(bytes.fromhex(case['bytes'])), case['length'])
        self.assertTrue(fixture_coverage(fixtures))

    def test_addresses_and_raw_bytes(self):
        output = '   0x2400:\te7 f1 5a 00\tmovb rh0,#0x5a\n   0x2404:\tnop\n'
        check_listing(output, [(0x2400, 'movb rh0,#0x5a', 'e7f15a00'),
                               (0x2404, 'nop', None)])
        with self.assertRaises(AssertionError):
            check_listing(output, [(0x2400, 'movb rh0,#0x5a', 'e7f15a00'),
                                   (0x2402, 'nop', None)])
        with self.assertRaises(AssertionError):
            check_listing(output, [(0x2400, 'movb rh0,#0x5a', 'e7f15aff'),
                                   (0x2404, 'nop', None)])

    def test_missing_or_extra_instruction_fails(self):
        with self.assertRaises(AssertionError):
            check_listing('  0x2400: nop\n', [(0x2400, 'nop'), (0x2402, 'nop')])
        with self.assertRaises(AssertionError):
            check_listing('  0x2400: nop\n  0x2402: nop\n', [(0x2400, 'nop')])


if __name__ == '__main__':
    unittest.main()
