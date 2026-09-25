"""Unit tests for summarize.py (python3 -m unittest)."""

import contextlib
import csv
import io
import json
import os
import tempfile
import unittest

import fixture
import summarize


class WindowsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        fixture.make_result_dir(self.tmp.name)
        self.runs, self.windows = summarize.collect(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_every_run_is_found(self):
        self.assertEqual(sorted(self.runs), sorted((m, r) for m in fixture.METHODS for r in (0, 100)))
        self.assertEqual(sorted(self.windows), sorted(self.runs))

    def test_window_query_statistics(self):
        ws = self.windows[("bitlsm_rho0.001", 100)]
        self.assertEqual([w["window"] for w in ws], [0, 1, 2])
        self.assertEqual([w["t_start_s"] for w in ws], [0, 2, 4])
        self.assertEqual([w["queries"] for w in ws], [4, 4, 4])
        self.assertEqual([w["qps"] for w in ws], [2.0, 2.0, 2.0])
        self.assertEqual(ws[0]["median_latency_ms"], 0.4)
        self.assertEqual(ws[0]["mean_matched"], 100.0)
        self.assertEqual(ws[2]["mean_matched"], 107.5)  # window 2 = seq 8 (pass 2, 100) + seq 9..11 (pass 3, 110): passes are 1.5 s, windows 2 s
        self.assertEqual(sum(w["queries"] for w in ws),
                         self.runs[("bitlsm_rho0.001", 100)]["queries_completed"])

    def test_window_writer_statistics(self):
        ws = self.windows[("bitlsm_rho0.001", 100)]
        self.assertEqual([w["updates_issued"] for w in ws], [200, 200, 200])
        self.assertEqual([w["backlog_end"] for w in ws], [0, 0, 0])
        # lag_mean_us is 10*(sec+1): window 1 covers sec 2,3 -> 35 (weighted by equal counts)
        self.assertEqual(ws[1]["lag_mean_us"], 35)
        self.assertEqual(ws[1]["put_mean_us"], 15)
        self.assertEqual(ws[1]["put_max_us"], 30)

    def test_window_lsm_deltas_and_levels(self):
        ws = self.windows[("bitlsm_rho0.001", 100)]
        # samples at t = 0..6 s; window w spans [2w, 2w+2) s and its end sample
        # is the last one before 2w+2 s, its start sample the last one before 2w s
        self.assertEqual([w["stall_delays"] for w in ws], [1, 2, 2])
        self.assertEqual([w["l0_files"] for w in ws], [1, 3, 5])
        self.assertEqual(ws[2]["pending_compaction_mb"], round(5000 * 1000 / 2**20, 1))
        self.assertEqual(ws[2]["running_compactions"], 0)

    def test_noload_run_has_no_writer_columns(self):
        ws = self.windows[("bitlsm_rho0.001", 0)]
        self.assertEqual(ws[0]["updates_issued"], "")
        self.assertEqual(ws[0]["backlog_end"], "")
        self.assertEqual(ws[0]["qps"], 2.0)


class RunsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def test_noload_ratio_and_totals(self):
        fixture.make_result_dir(self.tmp.name)
        runs, _ = summarize.collect(self.tmp.name)
        r = runs[("embedded-postings_il2", 100)]
        self.assertEqual(r["qps_total"], 2.0)
        self.assertEqual(r["qps_noload"], 2.0)
        self.assertEqual(r["qps_ratio"], 1.0)
        self.assertEqual(r["target_rate"], 100)
        self.assertEqual(r["actual_rate"], 100)
        self.assertEqual(r["queries_completed"], 12)
        self.assertEqual(r["in_flight_at_end"], 1)
        self.assertEqual(r["unissued"], 0)
        self.assertEqual(r["drain_ms"], 3)
        self.assertFalse(r["overloaded"])
        self.assertEqual(r["stall_delays"], 6)
        self.assertEqual(r["stall_stops"], 1)
        self.assertEqual(r["compact_write_gb"], round(6000 * 200 / 2**30, 4))
        self.assertAlmostEqual(r["matched_drift"], 0.1, places=6)
        self.assertEqual(runs[("embedded-postings_il2", 0)]["qps_ratio"], 1.0)
        self.assertEqual(runs[("embedded-postings_il2", 0)]["unissued"], "")

    def test_overloaded_by_actual_rate(self):
        fixture.make_result_dir(self.tmp.name, overloaded_method="bitlsm_rho0.001")
        runs, _ = summarize.collect(self.tmp.name)
        self.assertTrue(runs[("bitlsm_rho0.001", 100)]["overloaded"])
        self.assertFalse(runs[("embedded-postings_il2", 100)]["overloaded"])
        self.assertFalse(runs[("bitlsm_rho0.001", 0)]["overloaded"])

    def test_overloaded_by_rising_tail_backlog(self):
        fixture.make_result_dir(self.tmp.name, rising_method="embedded-postings_il0")
        runs, windows = summarize.collect(self.tmp.name)
        r = runs[("embedded-postings_il0", 200)]
        self.assertEqual(r["actual_rate"], 200)      # rate rule does not fire
        self.assertEqual(r["backlog_at_end"], 3)     # below W: backlog rule does not fire
        self.assertTrue(r["overloaded"])             # tail rule fires
        self.assertEqual([w["backlog_end"] for w in windows[("embedded-postings_il0", 200)]], [1, 2, 3])

    def test_backlog_over_one_second_is_overloaded(self):
        fixture.write_run(self.tmp.name, "bitlsm_rho0.001", 0)
        fixture.write_run(self.tmp.name, "bitlsm_rho0.001", 100, tail_backlog=(0, 0, 150))
        runs, _ = summarize.collect(self.tmp.name)
        self.assertTrue(runs[("bitlsm_rho0.001", 100)]["overloaded"])

    def test_backlog_at_end_uses_true_backlog_not_a_write_log_row_past_t(self):
        fixture.write_run(self.tmp.name, "bitlsm_rho0.001", 0)
        prefix = fixture.write_run(self.tmp.name, "bitlsm_rho0.001", 100)
        # A driver whose last Put finishes more than a second after T emits a
        # trailing sec == T row whose backlog is ScheduledBy(T+1) - issued --
        # roughly W too large -- which the old last-write-log-row logic would
        # have picked up as backlog_at_end.
        with open(prefix + "_write_log.csv", "a") as f:
            f.write("6,700,600,100,0,0,0,0,0\n")
        runs, _ = summarize.collect(self.tmp.name)
        r = runs[("bitlsm_rho0.001", 100)]
        self.assertEqual(r["backlog_at_end"], 0)
        self.assertFalse(r["overloaded"])

    def test_csvs_written_with_the_declared_columns(self):
        fixture.make_result_dir(self.tmp.name)
        runs, windows = summarize.collect(self.tmp.name)
        wpath, rpath = summarize.write_csvs(self.tmp.name, runs, windows)
        with open(wpath, newline="") as f:
            rows = list(csv.DictReader(f))
        self.assertEqual(list(rows[0].keys()), summarize.WINDOW_COLUMNS)
        self.assertEqual(len(rows), 3 * 2 * 3)
        with open(rpath, newline="") as f:
            rows = list(csv.DictReader(f))
        self.assertEqual(list(rows[0].keys()), summarize.RUN_COLUMNS)
        self.assertEqual(len(rows), 6)
        self.assertEqual(os.path.basename(wpath), "queries_under_concurrent_updates_windows.csv")
        self.assertEqual(os.path.basename(rpath), "queries_under_concurrent_updates_summary.csv")

    def test_failed_flag_is_read_from_totals(self):
        fixture.make_result_dir(self.tmp.name)
        prefix = os.path.join(self.tmp.name,
                              "read_seq_sel0.0001_k3_r300_bitlsm_rho0.001_w100")
        with open(prefix + "_meta.json") as f:
            meta = json.load(f)
        meta["totals"]["failed"] = True
        with open(prefix + "_meta.json", "w") as f:
            json.dump(meta, f)
        runs, _ = summarize.collect(self.tmp.name)
        self.assertTrue(runs[("bitlsm_rho0.001", 100)]["failed"])
        self.assertFalse(runs[("bitlsm_rho0.001", 0)]["failed"])


class EdgeCaseTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def test_last_window_includes_a_query_ending_exactly_at_t(self):
        prefix = fixture.write_run(self.tmp.name, "bitlsm_rho0.001", 0)
        with open(prefix + "_query_log.csv", "a") as f:
            f.write('12,4,0,3,"a,b,c",5999,6000,400,100,1\n')
        _, windows = summarize.collect(self.tmp.name)
        ws = windows[("bitlsm_rho0.001", 0)]
        self.assertEqual(ws[0]["queries"], 4)
        self.assertEqual(ws[1]["queries"], 4)
        self.assertEqual(ws[2]["queries"], 5)  # boundary row (end_ms=6000) counts;
        # the pre-existing end_ms=6400 row stays excluded (in_window=0)

    def test_windows_weight_lag_and_put_means_by_put_count(self):
        prefix = fixture.write_run(self.tmp.name, "bitlsm_rho0.001", 100)
        rows = [
            (0, 0, 0, 100),
            (1, 0, 0, 100),
            (2, 10, 10, 300),
            (3, 50, 50, 100),
            (4, 0, 0, 100),
            (5, 0, 0, 100),
        ]
        with open(prefix + "_write_log.csv", "w", newline="") as f:
            f.write("sec,scheduled_cum,issued_cum,backlog_end,lag_mean_us,"
                    "lag_max_us,put_mean_us,put_max_us,put_count\n")
            for sec, lag_mean, put_mean, put_count in rows:
                scheduled = 100 * (sec + 1)
                f.write(f"{sec},{scheduled},{scheduled},0,{lag_mean},20,"
                        f"{put_mean},30,{put_count}\n")
        _, windows = summarize.collect(self.tmp.name)
        ws = windows[("bitlsm_rho0.001", 100)]
        # sec 2 (count 300) and sec 3 (count 100): weighted mean = 20, not the
        # unweighted (10+50)/2 = 30
        self.assertEqual(ws[1]["lag_mean_us"], 20)
        self.assertEqual(ws[1]["put_mean_us"], 20)

    def test_window_with_no_completed_query(self):
        prefix = fixture.write_run(self.tmp.name, "bitlsm_rho0.001", 0)
        path = prefix + "_query_log.csv"
        with open(path, newline="") as f:
            reader = csv.reader(f)
            header = next(reader)
            rows = [r for r in reader if not (2000 <= int(r[6]) < 4000)]
        with open(path, "w", newline="") as f:
            writer = csv.writer(f)
            writer.writerow(header)
            writer.writerows(rows)
        _, windows = summarize.collect(self.tmp.name)
        ws = windows[("bitlsm_rho0.001", 0)]
        self.assertEqual(ws[0]["queries"], 4)
        self.assertEqual(ws[1]["queries"], 0)
        self.assertEqual(ws[1]["qps"], 0.0)
        self.assertEqual(ws[1]["median_latency_ms"], "")
        self.assertEqual(ws[1]["mean_matched"], "")
        self.assertEqual(ws[2]["queries"], 4)

    def test_missing_noload_run(self):
        fixture.write_run(self.tmp.name, "embedded-postings_il2", 100)
        runs, windows = summarize.collect(self.tmp.name)
        r = runs[("embedded-postings_il2", 100)]
        self.assertEqual(r["qps_noload"], "")
        self.assertEqual(r["qps_ratio"], "")
        wpath, rpath = summarize.write_csvs(self.tmp.name, runs, windows)
        with open(rpath, newline="") as f:
            rrows = list(csv.DictReader(f))
        self.assertEqual(len(rrows), 1)
        with open(wpath, newline="") as f:
            wrows = list(csv.DictReader(f))
        self.assertEqual(len(wrows), 3)


class CollectSkipsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def test_lazy_bitmaps_run_files_are_recognized(self):
        m = summarize.META_PATTERN.match(
            "read_seq_sel0.00001_k2_r300_lazy-bitmaps_rho0.001_w1000_meta.json")
        self.assertEqual((m.group("method"), m.group("rate")),
                         ("lazy-bitmaps_rho0.001", "1000"))

    def test_unrecognized_meta_file_is_skipped_and_reported(self):
        fixture.write_run(self.tmp.name, "bitlsm_rho0.001", 0)
        stray = os.path.join(self.tmp.name, "foo_no-index_w0_meta.json")
        with open(stray, "w") as f:
            f.write("{}")
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            runs, _ = summarize.collect(self.tmp.name)
        self.assertEqual(sorted(runs), [("bitlsm_rho0.001", 0)])
        self.assertIn("foo_no-index_w0_meta.json", stderr.getvalue())

    def test_sweep_meta_json_is_not_reported_as_unrecognized(self):
        fixture.make_result_dir(self.tmp.name)
        with open(os.path.join(self.tmp.name, summarize.SWEEP_META), "w") as f:
            json.dump({"started": "x"}, f)
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            runs, _ = summarize.collect(self.tmp.name)
        self.assertEqual(stderr.getvalue(), "")
        self.assertEqual(sorted(runs), sorted((m, r) for m in fixture.METHODS for r in (0, 100)))

    def test_malformed_meta_file_is_skipped_and_reported(self):
        fixture.write_run(self.tmp.name, "bitlsm_rho0.001", 0)
        bad_prefix = os.path.join(self.tmp.name,
                                  "read_seq_sel0.0001_k3_r300_bitlsm_rho0.001_w100")
        with open(bad_prefix + "_meta.json", "w") as f:
            f.write('{"update_rate": 100')
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            runs, _ = summarize.collect(self.tmp.name)
        self.assertEqual(sorted(runs), [("bitlsm_rho0.001", 0)])
        self.assertIn("read_seq_sel0.0001_k3_r300_bitlsm_rho0.001_w100_meta.json",
                      stderr.getvalue())


class CombinedTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.a = os.path.join(self.tmp.name, "a")
        self.b = os.path.join(self.tmp.name, "b")
        fixture.make_result_dir(self.a)
        fixture.make_result_dir(self.b)

    def tearDown(self):
        self.tmp.cleanup()

    def test_query_set_reads_the_predicate_count_from_the_file_names(self):
        # the fixture writes read_seq_sel0.0001_k3_r300_* names
        self.assertEqual(summarize.query_set(self.a), 3)

    def test_query_set_falls_back_to_the_directory_name(self):
        plain = os.path.join(self.tmp.name, "no_query_set")
        os.makedirs(plain)
        self.assertEqual(summarize.query_set(plain), "no_query_set")

    def test_combined_csvs_carry_every_sweep_with_a_c_column(self):
        collected = []
        for c, d in ((2, self.a), (3, self.b)):
            runs, windows = summarize.collect(d)
            collected.append((c, runs, windows))
        wpath, rpath = summarize.write_combined(self.b, collected)
        self.assertEqual(os.path.basename(wpath), "queries_under_concurrent_updates_windows_all.csv")
        self.assertEqual(os.path.basename(rpath), "queries_under_concurrent_updates_summary_all.csv")
        with open(rpath, newline="") as f:
            rows = list(csv.DictReader(f))
        self.assertEqual(list(rows[0].keys()), ["c"] + summarize.RUN_COLUMNS)
        self.assertEqual(len(rows), 12)  # 6 runs per directory
        self.assertEqual({r["c"] for r in rows}, {"2", "3"})
        with open(wpath, newline="") as f:
            wrows = list(csv.DictReader(f))
        self.assertEqual(list(wrows[0].keys()), ["c"] + summarize.WINDOW_COLUMNS)
        self.assertEqual(len(wrows), 2 * 6 * 3)


if __name__ == "__main__":
    unittest.main()
