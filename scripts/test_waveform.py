#!/usr/bin/env python3
"""Tests for scripts/waveform.py (run: python3 scripts/test_waveform.py)."""
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import waveform as wf  # noqa: E402


class EncodeTest(unittest.TestCase):
    def test_round_trip_matches_stock_settle_bytes(self):
        # kXtfPreBwMid 0x22 row from the driver: 01 86 81 86 86 01 01 | 01 82 84 00 00 01 01
        phases = [[2, 6], [2, 1], [2, 6], [2, 6], [2, 2], [2, 4]]
        data = wf.encode_table(phases, 42)
        self.assertEqual(data[:14].hex(), "01868186860101" + "01828400000101")
        self.assertEqual(wf.decode_table(data), phases)

    def test_rejects_rail_3_and_long_phases(self):
        with self.assertRaises(wf.SpecError):
            wf.encode_table([[3, 1]], 49)
        with self.assertRaises(wf.SpecError):
            wf.encode_table([[1, 64]], 49)

    def test_rejects_too_many_phases(self):
        with self.assertRaises(wf.SpecError):
            wf.encode_table([[0, 1]] * 25, 42)  # 6 groups x 4 phases = 24


class SpecTest(unittest.TestCase):
    def write(self, spec):
        f = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, dir=Path(__file__).parent)
        json.dump(spec, f)
        f.close()
        self.addCleanup(Path(f.name).unlink)
        return f.name

    def test_direct_needs_all_five_tables(self):
        path = self.write({"bank": "direct", "tables": {"0x20": [[0, 5]], "0x21": [[2, 5]]}})
        with self.assertRaises(wf.SpecError):
            wf.load_spec(path)

    def test_frame_cap(self):
        path = self.write({"bank": "overlay", "tables": {"0x21": [[1, 30], [2, 30]]}})
        with self.assertRaises(wf.SpecError):
            wf.load_spec(path)

    def test_candidate_tables_end_together_and_fit(self):
        spec = wf.candidate_spec([0x23, 0x21, 0x22, 0x24], 3)
        path = self.write(spec)
        _, tables = wf.load_spec(path)
        self.assertEqual({st["frames"] for _, _, st in tables.values()}, {28})
        self.assertEqual(spec["tables"]["0x23"], [[0, 28]])  # code 0 (hold) idles

    def test_shipped_specs_are_valid(self):
        for path in sorted((Path(__file__).parent / "waveforms").glob("*.json")):
            with self.subTest(path.name):
                wf.load_spec(str(path))


if __name__ == "__main__":
    unittest.main()
