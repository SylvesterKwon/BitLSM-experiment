"""Unit tests for plot_latency_vs_w.py (python3 -m unittest). Runs headless (Agg)."""

import math
import os
import tempfile
import unittest

import matplotlib
matplotlib.use("Agg")

import fixture
import plot_latency_vs_w as pq


class FigureTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.a = os.path.join(self.tmp.name, "a")
        self.b = os.path.join(self.tmp.name, "b")
        fixture.make_result_dir(self.a)
        fixture.make_result_dir(self.b, rising_method="bitlsm_rho0.001")

    def tearDown(self):
        self.tmp.cleanup()

    def test_one_panel_per_result_dir_on_its_own_linear_axis(self):
        fig = pq.make_figure([self.a, self.b], titles=["c = 2", "c = 3"])
        self.assertEqual(len(fig.axes), 2)
        for ax in fig.axes:
            self.assertEqual(ax.get_yscale(), "linear")
            self.assertEqual(ax.get_ylim()[0], 0)
        # Each panel is a different query set, so the y axes are independent.
        self.assertFalse(fig.axes[0].get_shared_y_axes().joined(fig.axes[0], fig.axes[1]))
        self.assertEqual([t.get_text() for t in fig.axes[0].get_xticklabels()], ["0", ""])
        self.assertEqual([t.get_text() for t in fig.axes[1].get_xticklabels()], ["0", "", "200"])
        self.assertEqual(fig.get_size_inches()[0], 3.333)

    def test_panel_with_a_wide_gap_gets_a_broken_axis(self):
        gap = os.path.join(self.tmp.name, "gap")
        os.makedirs(gap)
        for m in fixture.METHODS:
            slow = 4000 if m == "bitlsm_rho0.001" else 400  # ten times the others
            fixture.write_run(gap, m, 0, latency_us=slow)
            fixture.write_run(gap, m, 100, latency_us=slow)
        fig = pq.make_figure([gap])
        upper, lower = fig.axes
        self.assertEqual(lower.get_ylim()[0], 0)
        self.assertGreater(upper.get_ylim()[0], lower.get_ylim()[1])
        self.assertTrue(lower.get_ylim()[1] >= 0.4 and upper.get_ylim()[0] <= 4.0)
        self.assertFalse(upper.spines["bottom"].get_visible())
        self.assertFalse(lower.spines["top"].get_visible())
        self.assertEqual(upper.get_title(), "c = 3")

    def test_latency_is_plotted_in_milliseconds(self):
        import summarize
        runs, _ = summarize.collect(self.a)
        fig = pq.make_figure([self.a])
        line = {l.get_label(): l for l in fig.axes[0].get_lines()}["BitLSM"]
        self.assertEqual(list(line.get_ydata()),
                         [float(runs[("bitlsm_rho0.001", w)]["mean_latency_ms"]) for w in (0, 100)])
        self.assertEqual(fig.texts[0].get_text(), "Mean query latency (ms)")

    def test_overloaded_run_is_drawn_hollow(self):
        over = os.path.join(self.tmp.name, "over")
        fixture.make_result_dir(over, overloaded_method="bitlsm_rho0.001")
        fig = pq.make_figure([over])
        hollow = [l for l in fig.axes[0].get_lines()
                  if l.get_markerfacecolor() == "white" and l.get_markeredgecolor() == "#E04040"]
        self.assertEqual(len(hollow), 1)
        self.assertEqual(list(hollow[0].get_xdata()), [1])  # W = 100 is the second rate

    def test_titles_default_to_the_query_set(self):
        # fixture writes read_seq_sel0.0001_k3_r300_* file names
        self.assertEqual(pq.panel_title(self.a), "c = 3")

    def test_rate_label_writes_values_out_and_uses_k_when_long(self):
        self.assertEqual([pq.rate_label(w) for w in (0, 100, 1000, 16000, 128000)],
                         ["0", "100", "1k", "16k", "128k"])

    def test_series_carry_the_paper_colours_and_styles(self):
        fig = pq.make_figure([self.a])
        lines = {l.get_label(): l for l in fig.axes[0].get_lines()}
        self.assertEqual(lines["BitLSM"].get_color(), "#E04040")
        self.assertEqual(lines["Embedded Postings (Intersection)"].get_color(), "#2F6FD0")
        self.assertEqual(lines["Embedded Postings (Intersection)"].get_linestyle(), "--")
        self.assertEqual(lines["Embedded Postings (Top-2 Intersection)"].get_linestyle(), "-")

    def test_legend_lists_baselines_first_and_is_drawn_once(self):
        fig = pq.make_figure([self.a, self.b])
        self.assertEqual(len(fig.legends), 1)
        self.assertEqual([t.get_text() for t in fig.legends[0].get_texts()],
                         ["Embedded Postings (Top-2 Intersection)",
                          "Embedded Postings (Intersection)", "BitLSM"])
        self.assertFalse(fig.legends[0].get_frame_on())

    def test_no_runs_gives_no_figure(self):
        empty = os.path.join(self.tmp.name, "empty")
        os.makedirs(empty)
        self.assertIsNone(pq.make_figure([empty]))


if __name__ == "__main__":
    unittest.main()
