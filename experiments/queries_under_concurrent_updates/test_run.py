"""Unit tests for the queries_under_concurrent_updates sweep runner (python3 -m unittest)."""

import hashlib
import os
import tempfile
import unittest
import unittest.mock

import run


def _write(path, content=b"x"):
    with open(path, "wb") as f:
        f.write(content)


class ClassifyTest(unittest.TestCase):
    def test_file_classes(self):
        self.assertEqual(run.classify("000123.sst"), "link")
        for n in ("CURRENT", "IDENTITY", "MANIFEST-002149", "OPTIONS-000007", "002148.log"):
            self.assertEqual(run.classify(n), "copy", n)
        for n in ("LOCK", "LOG", "LOG.old.1789706391733777"):
            self.assertEqual(run.classify(n), "skip", n)
        self.assertIsNone(run.classify("notes.txt"))


class CheckpointTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = os.path.join(self.tmp.name, "base")
        os.makedirs(self.base)
        self.files = {
            "000010.sst": b"sst-data-1", "000011.sst": b"sst-data-2",
            "CURRENT": b"MANIFEST-000012\n", "IDENTITY": b"id",
            "MANIFEST-000012": b"manifest", "OPTIONS-000007": b"opts",
            "000013.log": b"", "LOG": b"log", "LOG.old.42": b"old", "LOCK": b"",
        }
        for n, c in self.files.items():
            _write(os.path.join(self.base, n), c)
        self.run_db = os.path.join(self.tmp.name, "runs", "r1")

    def tearDown(self):
        self.tmp.cleanup()

    def test_links_ssts_copies_metadata_skips_per_open_files(self):
        before = run.inventory(self.base)
        counts = run.checkpoint_db(self.base, self.run_db)
        self.assertEqual(counts, {"linked": 2, "copied": 5, "skipped": 3})
        for n in ("000010.sst", "000011.sst"):
            self.assertEqual(os.stat(os.path.join(self.base, n)).st_ino,
                             os.stat(os.path.join(self.run_db, n)).st_ino, n)
        for n in ("CURRENT", "IDENTITY", "MANIFEST-000012", "OPTIONS-000007", "000013.log"):
            self.assertNotEqual(os.stat(os.path.join(self.base, n)).st_ino,
                                os.stat(os.path.join(self.run_db, n)).st_ino, n)
            with open(os.path.join(self.run_db, n), "rb") as f:
                self.assertEqual(f.read(), self.files[n])
        for n in ("LOG", "LOG.old.42", "LOCK"):
            self.assertFalse(os.path.exists(os.path.join(self.run_db, n)), n)
        self.assertEqual(run.inventory(self.base), before)

    def test_inventory_lists_every_file_with_size(self):
        inv = run.inventory(self.base)
        self.assertEqual([n for n, _ in inv], sorted(self.files))
        self.assertEqual(dict(inv)["000010.sst"], len(b"sst-data-1"))
        self.assertEqual(len(run.inventory_sha256(inv)), 64)

    def test_unknown_file_aborts_before_creating_anything(self):
        _write(os.path.join(self.base, "stray.txt"))
        with self.assertRaises(RuntimeError):
            run.checkpoint_db(self.base, self.run_db)
        self.assertFalse(os.path.exists(self.run_db))

    def test_existing_run_db_aborts(self):
        os.makedirs(self.run_db)
        with self.assertRaises(RuntimeError):
            run.checkpoint_db(self.base, self.run_db)

    def test_partial_copy_is_cleaned_up_on_failure(self):
        # The .sst files sort before the metadata files, so copy2 fails
        # after both SSTs were already linked into run_db.
        with unittest.mock.patch("run.shutil.copy2", side_effect=OSError("disk full")):
            with self.assertRaises(OSError):
                run.checkpoint_db(self.base, self.run_db)
        self.assertFalse(os.path.exists(self.run_db))


class NamingAndCommandTest(unittest.TestCase):
    def test_run_db_name_carries_every_flag(self):
        self.assertEqual(run.run_db_name("bitlsm", {"rho": 0.001}, 0), "bitlsm_rho0.001_w0")
        self.assertEqual(run.run_db_name("embedded-postings", {"intersection_limit": 2}, 1000),
                         "embedded-postings_intersection_limit2_w1000")
        self.assertEqual(run.run_db_name("no-index", {}, 0.5), "no-index_default_w0.5")

    def test_encode_method_params_names_the_base_db(self):
        self.assertEqual(run.encode_method_params({"rho": 0.001}), "rho0.001")
        self.assertEqual(run.encode_method_params({"intersection_limit": 2}), "default")

    def test_build_command(self):
        cfg = {"query_workload": "/q.tsv", "update_workload": "/u.tsv",
               "duration_s": 1800, "window_s": 60, "sample_interval_s": 1, "seed": 42,
               "common_params": {"indexed_attrs": "a,b", "scan_prefetch_depth": 32}}
        cmd = run.build_command("bitlsm", {"rho": 0.001}, 1000, "/run/db", cfg, "/out")
        self.assertEqual(cmd[0], run.BINARY)
        s = " ".join(cmd)
        self.assertIn("--binding bitlsm", s)
        self.assertIn("--db_path /run/db", s)
        self.assertIn("--update_rate 1000 ", s)
        self.assertIn("--update_workload /u.tsv", s)
        self.assertIn("--indexed_attrs a,b", s)
        self.assertIn("--scan_prefetch_depth 32", s)
        self.assertIn("--rho 0.001", s)
        self.assertIn("--output_dir /out", s)
        noload = " ".join(run.build_command("bitlsm", {"rho": 0.001}, 0, "/run/db", cfg, "/out"))
        self.assertNotIn("--update_workload", noload)
        self.assertIn("--update_rate 0 ", noload)


class ShaTest(unittest.TestCase):
    def test_sidecar_log_is_preferred_over_recomputing(self):
        with tempfile.TemporaryDirectory() as d:
            tsv = os.path.join(d, "u.tsv")
            _write(tsv, b"u\tpk\t{}\n")
            self.assertEqual(run.tsv_sha256(tsv),
                             (hashlib.sha256(b"u\tpk\t{}\n").hexdigest(), "computed"))
            _write(os.path.join(d, "u.log"),
                   b"source_tsv=x num_updates=1 seed=1 mode=overwrite elapsed_s=0.0 sha256=abc123\n")
            self.assertEqual(run.tsv_sha256(tsv), ("abc123", "sidecar"))


if __name__ == "__main__":
    unittest.main()
