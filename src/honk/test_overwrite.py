"""Unit tests for the uniform-overwrite update generator (python3 -m unittest)."""

import json
import os
import tempfile
import unittest

from honk import overwrite


SOURCE_LINES = [
    'w\tpk-a\t{"PULocationID": 1, "fare_amount": 10.0}',
    'w\tpk-b\t{"PULocationID": 2, "fare_amount": 20.0}',
    'r\t{"filters": []}',  # must be ignored: not a write line
    'w\tpk-c\t{"PULocationID": 3, "fare_amount": 30.0}',
    'w\tpk-d\t{"PULocationID": 4, "fare_amount": 40.0}',
    'w\tpk-e\t{"PULocationID": 5, "fare_amount": 50.0}',
]
PKS = {"pk-a", "pk-b", "pk-c", "pk-d", "pk-e"}
JSONS = {l.split("\t")[2] for l in SOURCE_LINES if l.startswith("w\t")}


class OverwriteTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.src = os.path.join(self.tmp.name, "src.tsv")
        with open(self.src, "w") as f:
            f.write("\n".join(SOURCE_LINES) + "\n")

    def tearDown(self):
        self.tmp.cleanup()

    def test_count_write_lines_skips_reads(self):
        self.assertEqual(overwrite.count_write_lines(self.src), 5)

    def test_overwrite_lines_copy_whole_rows_onto_existing_keys(self):
        lines = list(overwrite.generate_lines(self.src, 40, seed=1))
        self.assertEqual(len(lines), 40)
        for line in lines:
            op, pk, js = line.rstrip("\n").split("\t")
            self.assertEqual(op, "u")
            self.assertIn(pk, PKS)
            self.assertIn(js, JSONS)
        # With replacement over 5 keys, 40 draws hit more than one key and
        # more than one payload row.
        self.assertGreater(len({l.split("\t")[1] for l in lines}), 1)
        self.assertGreater(len({l.split("\t")[2] for l in lines}), 1)

    def test_seed_determinism(self):
        a = list(overwrite.generate_lines(self.src, 30, seed=7))
        b = list(overwrite.generate_lines(self.src, 30, seed=7))
        c = list(overwrite.generate_lines(self.src, 30, seed=8))
        self.assertEqual(a, b)
        self.assertNotEqual(a, c)

    def test_insert_mode_uses_fresh_uuids(self):
        lines = list(overwrite.generate_lines(self.src, 10, seed=3, mode="insert"))
        pks = set()
        for line in lines:
            op, pk, js = line.rstrip("\n").split("\t")
            self.assertEqual(op, "w")
            self.assertNotIn(pk, PKS)
            self.assertEqual(len(pk), 36)  # uuid4 text
            self.assertIn(js, JSONS)
            pks.add(pk)
        self.assertEqual(len(pks), 10)

    def test_write_updates_from_config(self):
        cfg = os.path.join(self.tmp.name, "ovw_test.json")
        with open(cfg, "w") as f:
            json.dump({"source_tsv": self.src, "num_updates": 12, "seed": 5,
                       "mode": "overwrite"}, f)
        out_dir = os.path.join(self.tmp.name, "out")
        out = overwrite.write_updates(cfg, out_dir)
        self.assertEqual(out, os.path.join(out_dir, "ovw_test.tsv"))
        with open(out) as f:
            lines = f.readlines()
        self.assertEqual(len(lines), 12)
        self.assertTrue(all(l.startswith("u\t") for l in lines))
        with open(os.path.join(out_dir, "ovw_test.log")) as f:
            log = f.read()
        self.assertIn("sha256=", log)
        self.assertIn("num_updates=12", log)


if __name__ == "__main__":
    unittest.main()
