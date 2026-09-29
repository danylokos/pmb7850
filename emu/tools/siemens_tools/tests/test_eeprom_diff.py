#!/usr/bin/env python3
"""EEPROM inventory and byte-diff tests."""

from .eeprom_fixtures import *  # noqa: F401,F403


class DiffTests(unittest.TestCase):
    """diff_sources on synthetic inventories."""

    @staticmethod
    def _blk(bid: int, payload: bytes) -> Block:
        return Block(bid, payload, len(payload), "map")

    def test_counts_and_first_diff(self) -> None:
        a = {1: self._blk(1, b"\x00\x01\x02"),
             2: self._blk(2, b"same"),
             3: self._blk(3, b"only-a")}
        b = {1: self._blk(1, b"\x00\xFF\x02"),  # differ at offset 1
             2: self._blk(2, b"same"),          # equal
             4: self._blk(4, b"only-b")}
        r = diff_sources(a, b)
        self.assertEqual(r.only_in_a, [3])
        self.assertEqual(r.only_in_b, [4])
        self.assertEqual(r.equal_count, 1)
        self.assertEqual(r.common, 2)
        self.assertEqual([(d.block_id, d.first_diff) for d in r.differ], [(1, 1)])

    def test_length_mismatch_first_diff_at_shorter_end(self) -> None:
        a = {1: self._blk(1, b"abc")}
        b = {1: self._blk(1, b"abcd")}  # common prefix, b is longer
        r = diff_sources(a, b)
        self.assertEqual(r.differ[0].len_a, 3)
        self.assertEqual(r.differ[0].len_b, 4)
        self.assertEqual(r.differ[0].first_diff, 3)

    def test_version_only_change_is_reported(self) -> None:
        a = {5372: Block(5372, b"same", 4, "map",
                         memory_class=8, version=1)}
        b = {5372: Block(5372, b"same", 4, "dump",
                         memory_class=8, version=0)}
        result = diff_sources(a, b)
        self.assertEqual(result.equal, [])
        self.assertIsNone(result.differ[0].first_diff)
        self.assertEqual((result.differ[0].version_a,
                          result.differ[0].version_b), (1, 0))






class ByteDiffRenderTests(unittest.TestCase):
    def test_color_wraps_equal_green_and_diff_red(self) -> None:
        out = render_block_bytediff(bytes([0x10, 0x20]), bytes([0x10, 0xFF]), color=True)
        # 0x10 matches -> green; 0x20 vs 0xFF differ -> red present.
        self.assertIn("\x1b[32m10\x1b[0m", out)
        self.assertIn("\x1b[31m", out)

    def test_no_color_has_no_ansi(self) -> None:
        out = render_block_bytediff(bytes([0x10, 0x20]), bytes([0x10, 0xFF]), color=False)
        self.assertNotIn("\x1b[", out)
        self.assertIn("10", out)

    def test_length_mismatch_padding(self) -> None:
        out = render_block_bytediff(bytes([0x10]), bytes([0x10, 0x20]), color=False)
        self.assertIn("--", out)  # missing byte on the shorter (A) side




if __name__ == "__main__":
    unittest.main()
