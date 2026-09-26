#!/usr/bin/env python3
"""Mean query latency against the update rate, one panel per query set.

The paper figure for this experiment: x = the swept update rates (categorical,
so the measurement points are evenly spaced), y = the arithmetic mean of the
per-query latencies over the 30 min run, one line per method, one panel per
predicate count. The mean, not the median the query-performance section uses:
the update stream lengthens the tail, and the mean is what the closed-loop
worker's throughput inverts to.

Unlike the other figures here the panels do NOT share a y axis: each panel is a
different query set, so there is nothing to compare across them, and a shared
axis would only squeeze the slower methods of the wider-spread panel. The axes
are linear, in milliseconds and start at zero, which keeps "how much does
ingestion cost a query" readable; ratios between methods are reported in the
text. A panel whose arms leave a gap wider than BREAK_RATIO (c = 2: Lazy
Bitmaps at 300-700 ms over the rest at 30-80 ms) is drawn on a broken y axis,
so the arms below keep their resolution. A run whose writer could not sustain
its update rate (`overloaded` in the summary) is drawn as a hollow marker.

Reads the runs through summarize.collect(), so the figure and runs.csv are one
computation.

Usage:
    python3 experiments/queries_under_concurrent_updates/plot_latency_vs_w.py <result_dir> [<result_dir> ...]
    python3 experiments/queries_under_concurrent_updates/plot_latency_vs_w.py <dir> <dir> -o <out.png>
"""

import argparse
import math
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as mt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import summarize  # noqa: E402

PANEL_BOX_ASPECT = 0.702

plt.rcParams.update({
    "font.size": 6,
    "xtick.direction": "in", "ytick.direction": "in",
    "xtick.major.size": 2, "ytick.major.size": 2,
    "xtick.minor.size": 1, "ytick.minor.size": 1,
    "xtick.major.width": 0.3, "ytick.major.width": 0.3,
    "xtick.minor.width": 0.3, "ytick.minor.width": 0.3,
})

# Baselines first, ours last. Where a sweep has only one Embedded Postings arm
# (c = 2, where top-2 of two predicates keeps both and the two strategies are
# the same plan) it is the shipped top-2 default and draws as that series.
SERIES = [("lazy-bitmaps_rho0.001", "Lazy Bitmaps", "#C0409A", "-"),
          ("embedded-postings_il2", "Embedded Postings (Top-2 Intersection)", "#2F6FD0", "-"),
          ("embedded-postings_il0", "Embedded Postings (Intersection)", "#2F6FD0", "--"),
          ("bitlsm_rho0.001", "BitLSM", "#E04040", "-")]

# Row geometry: pin the panel box and give the figure the box plus the
# furniture it carries (legend and titles above, tick labels and the shared x
# label below), as in memory_pressure/plot.py.
FIG_W = 3.333          # one paper column
LEFT, RIGHT, WSPACE = 0.17, 0.965, 0.26
TICKS_H, XLABEL_H = 0.14, 0.17
TITLES_H = 0.13
# Two legend columns; the band grows with the row count so the panel box
# below it stays put.
LEGEND_NCOL = 2
LEGEND_ROW_H, LEGEND_PAD = 0.105, 0.17
# Legend order, column-major: the two Embedded Postings arms share the first
# column so both full names fit one paper column (a space exception to the
# family order); Lazy Bitmaps and ours take the second, ours last.
LEGEND_ORDER = ("embedded-postings_il2", "embedded-postings_il0",
                "lazy-bitmaps_rho0.001", "bitlsm_rho0.001")
LEGEND_HANDLE_LEN = 2.2  # just long enough for the dashed arm to read as dashed
# A panel whose arms leave a gap wider than this ratio between the slowest
# arm below and the fastest arm above is drawn on a broken y axis, so the
# arms below keep their resolution. The break is marked on both axes.
BREAK_RATIO = 3.0
BREAK_HEIGHTS = (1.0, 1.2)  # upper : lower share of the panel box
BREAK_GAP = 0.10            # vertical gap between the two halves


def panel_title(result_dir):
    """`c = N` from the query set the sweep replayed, else the directory's
    own name (summarize.query_set does the reading)."""
    c = summarize.query_set(result_dir)
    return f"c = {c}" if isinstance(c, int) else str(c)


def rate_label(rate):
    """Written out, in thousands once the numbers get long."""
    if rate == 0:
        return "0"
    return f"{rate // 1000}k" if rate >= 1000 else str(rate)


def make_figure(result_dirs, titles=None):
    """One panel per result directory. Returns the Figure, or None if no
    directory held a run."""
    panels = []
    for i, d in enumerate(result_dirs):
        runs, _ = summarize.collect(d)
        if runs:
            panels.append((titles[i] if titles else panel_title(d), runs))
    if not panels:
        return None

    ncols = len(panels)
    box_w = FIG_W * (RIGHT - LEFT) / (ncols + (ncols - 1) * WSPACE)
    box_h = box_w * PANEL_BOX_ASPECT + 0.01
    present = {k for _, runs in panels for (k, _) in runs}
    n_series = sum(1 for key, *_ in SERIES if key in present)
    legend_h = LEGEND_PAD + LEGEND_ROW_H * math.ceil(n_series / LEGEND_NCOL)
    above, below = legend_h + TITLES_H, TICKS_H + XLABEL_H
    fig_h = box_h + above + below

    fig = plt.figure(figsize=(FIG_W, fig_h))
    grid = fig.add_gridspec(1, ncols, left=LEFT, right=RIGHT, wspace=WSPACE,
                            bottom=below / fig_h, top=1.0 - above / fig_h)
    for slot, (title, runs) in zip(grid, panels):
        _draw_panel(fig, slot, title, runs)
    fig.supylabel("Mean query latency (ms)", fontsize=6, x=0.012)
    fig.supxlabel("Update rate (updates/s)", fontsize=6, y=0.015)
    _legend(fig)
    return fig


def _arms(runs):
    """[(points, colour, style, label)] for every SERIES arm the sweep holds;
    a point is (rate index, mean latency in ms, overloaded)."""
    rates = sorted({w for (_, w) in runs})
    arms = []
    for key, label, colour, style in SERIES:
        # A method may be absent from a sweep, or from one of its rates.
        pts = [(i, float(runs[(key, w)]["mean_latency_ms"]),
                str(runs[(key, w)].get("overloaded")) == "True")
               for i, w in enumerate(rates) if (key, w) in runs]
        if pts:
            arms.append((pts, colour, style, label))
    return rates, arms


def _draw_panel(fig, slot, title, runs):
    """One panel in its grid slot: a single axes, or an upper and a lower half
    when the arms leave a gap (see _gap)."""
    rates, arms = _arms(runs)
    top = max(y for pts, *_ in arms for _, y, _ in pts) * 1.08
    gap = _gap(arms)
    if gap is None:
        axes = [fig.add_subplot(slot)]
        axes[0].set_ylim(0, top)
    else:
        lower_max, upper_min = gap
        sub = slot.subgridspec(2, 1, height_ratios=BREAK_HEIGHTS, hspace=BREAK_GAP)
        upper = fig.add_subplot(sub[0])
        lower = fig.add_subplot(sub[1], sharex=upper)
        axes = [upper, lower]
        upper.set_ylim(upper_min / 1.15, top)
        lower.set_ylim(0, lower_max * 1.15)
        _mark_break(upper, lower)
    for ax in axes:
        for pts, colour, style, label in arms:
            ax.plot([i for i, _, _ in pts], [y for _, y, _ in pts], marker="o",
                    markersize=2, linewidth=0.9, color=colour, linestyle=style, label=label)
            # A run whose writer could not sustain W is drawn hollow: its x is
            # the target rate, not the rate the method actually saw.
            over = [(i, y) for i, y, o in pts if o]
            if over:
                ax.plot([i for i, _ in over], [y for _, y in over], linestyle="none",
                        marker="o", markersize=2.6, markerfacecolor="white",
                        markeredgecolor=colour, markeredgewidth=0.7)
        ax.set_xticks(list(range(len(rates))))
        # Every rate gets a tick; labelling every other one keeps them apart.
        ax.set_xticklabels([rate_label(w) if i % 2 == 0 else "" for i, w in enumerate(rates)])
        ax.set_xlim(-0.35, len(rates) - 0.65)
        ax.yaxis.set_major_locator(mt.MaxNLocator(nbins=4, steps=[1, 2, 2.5, 5, 10]))
        ax.yaxis.set_major_formatter(mt.FuncFormatter(lambda v, _: f"{v:g}"))
        ax.grid(False)
    axes[0].set_title(title)
    return axes


def _gap(arms):
    """(lower_max, upper_min) of the widest gap between the arms' latency
    ranges when it exceeds BREAK_RATIO, else None. Arms are ordered by their
    lowest latency; a gap is measured from the slowest point below it to the
    fastest point above it."""
    ranges = sorted((min(y for _, y, _ in pts), max(y for _, y, _ in pts)) for pts, *_ in arms)
    best = None
    lower_max = ranges[0][1]
    for lo, hi in ranges[1:]:
        if lo > lower_max * BREAK_RATIO and (best is None or lo / lower_max > best[1] / best[0]):
            best = (lower_max, lo)
        lower_max = max(lower_max, hi)
    return best


def _mark_break(upper, lower):
    """Hide the spines that face the gap, keep x labels on the lower half and
    draw the diagonal break marks on both halves."""
    upper.spines["bottom"].set_visible(False)
    lower.spines["top"].set_visible(False)
    upper.tick_params(axis="x", which="both", bottom=False, labelbottom=False)
    d = 0.015
    for ax, y0 in ((upper, 0.0), (lower, 1.0)):
        kw = dict(transform=ax.transAxes, color="k", clip_on=False, linewidth=0.5)
        ax.plot((-d, d), (y0 - 2 * d, y0 + 2 * d), **kw)
        ax.plot((1 - d, 1 + d), (y0 - 2 * d, y0 + 2 * d), **kw)


def _legend(fig):
    """One legend for the figure, drawn once, in LEGEND_ORDER."""
    handles, labels = [], []
    for ax in fig.axes:
        for h, l in zip(*ax.get_legend_handles_labels()):
            if l not in labels:
                handles.append(h)
                labels.append(l)
    label_of = {key: label for key, label, _, _ in SERIES}
    order = [labels.index(label_of[k]) for k in LEGEND_ORDER if label_of[k] in labels]
    fig.legend([handles[i] for i in order], [labels[i] for i in order],
               loc="upper center", frameon=False, fontsize=6, ncol=LEGEND_NCOL,
               bbox_to_anchor=(0.5, 1.0), columnspacing=1.0,
               handlelength=LEGEND_HANDLE_LEN, handletextpad=0.5)


def main():
    ap = argparse.ArgumentParser(
        description="Plot mean query latency against the update rate, one panel per result directory")
    ap.add_argument("result_dirs", nargs="+")
    ap.add_argument("-o", "--output", default=None,
                    help="Output file (default: <last result_dir>/queries_under_concurrent_updates_latency_vs_w.pdf)")
    ap.add_argument("--titles", default=None,
                    help="Comma-separated panel titles (default: c = N from the query set)")
    args = ap.parse_args()
    titles = args.titles.split(",") if args.titles else None
    if titles and len(titles) != len(args.result_dirs):
        sys.exit("--titles needs one title per result directory")
    fig = make_figure(args.result_dirs, titles)
    if fig is None:
        print("No runs found.")
        return
    out = args.output or os.path.join(args.result_dirs[-1], "queries_under_concurrent_updates_latency_vs_w.pdf")
    fig.savefig(out, dpi=150)
    print(f"Saved: {out}")


if __name__ == "__main__":
    main()
