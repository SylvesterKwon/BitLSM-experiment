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
are linear and start at zero, which keeps "how much throughput does ingestion
cost" readable; ratios between methods are reported in the text instead.

Reads the runs through summarize.collect(), so the figure and runs.csv are one
computation.

Usage:
    python3 experiments/queries_under_concurrent_updates/plot_latency_vs_w.py <result_dir> [<result_dir> ...]
    python3 experiments/queries_under_concurrent_updates/plot_latency_vs_w.py <dir> <dir> -o <out.png>
"""

import argparse
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
LATENCY_TICKS_MS = (1, 2, 3, 5, 10, 20, 30, 50, 100, 200, 300, 500, 1000, 2000, 3000, 5000)
TITLES_H = 0.13
# The legend is one column, one row per series drawn, so its band grows with
# the series count and the panel box below it stays put.
LEGEND_ROW_H, LEGEND_PAD = 0.105, 0.17


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
        if not runs:
            continue
        title = titles[i] if titles else panel_title(d)
        panels.append((title, runs))
    if not panels:
        return None

    ncols = len(panels)
    box_w = FIG_W * (RIGHT - LEFT) / (ncols + (ncols - 1) * WSPACE)
    box_h = box_w * PANEL_BOX_ASPECT + 0.01
    n_series = sum(1 for key, _, _, _ in SERIES
                   if any(k == key for _, runs in panels for (k, _) in runs))
    legend_h = LEGEND_PAD + LEGEND_ROW_H * n_series
    above, below = legend_h + TITLES_H, TICKS_H + XLABEL_H
    fig_h = box_h + above + below

    fig, axes = plt.subplots(1, ncols, figsize=(FIG_W, fig_h), squeeze=False)
    for ax, (title, runs) in zip(axes[0], panels):
        rates = sorted({w for (_, w) in runs})
        xs = list(range(len(rates)))
        ys = []
        for key, label, colour, style in SERIES:
            # A method may be absent from a sweep, or from one of its rates.
            pts = [(i, float(runs[(key, w)]["mean_latency_ms"]),
                    str(runs[(key, w)].get("overloaded")) == "True")
                   for i, w in enumerate(rates) if (key, w) in runs]
            if not pts:
                continue
            ys += [y for _, y, _ in pts]
            ax.plot([i for i, _, _ in pts], [y for _, y, _ in pts], marker="o",
                    markersize=2, linewidth=0.9, color=colour, linestyle=style, label=label)
            # A run whose writer could not sustain W is drawn hollow: its
            # x is the target rate, not the rate the method actually saw.
            over = [(i, y) for i, y, o in pts if o]
            if over:
                ax.plot([i for i, _ in over], [y for _, y in over], linestyle="none",
                        marker="o", markersize=2.6, markerfacecolor="white",
                        markeredgecolor=colour, markeredgewidth=0.7)
        # Log axis: the arms sit an order of magnitude apart, and the vertical
        # distance between two lines is then their ratio. Ticks are written
        # out in ms at the values that fall inside the panel's range.
        ax.set_yscale("log")
        lo, hi = min(ys) / 1.15, max(ys) * 1.15
        ax.set_ylim(lo, hi)
        ax.set_yticks([t for t in LATENCY_TICKS_MS if lo <= t <= hi])
        ax.set_yticks([], minor=True)
        ax.set_xticks(xs)
        # Every rate gets a tick; labelling every other one keeps them apart.
        ax.set_xticklabels([rate_label(w) if i % 2 == 0 else "" for i, w in enumerate(rates)])
        ax.set_xlim(-0.35, len(rates) - 0.65)
        ax.yaxis.set_major_formatter(mt.FuncFormatter(lambda v, _: f"{v:g}"))
        ax.set_title(title)
        ax.grid(False)
        ax.set_box_aspect(PANEL_BOX_ASPECT)

    fig.supylabel("Mean query latency (ms)", fontsize=6, x=0.012)
    fig.supxlabel("Update rate (updates/s)", fontsize=6, y=0.015)

    handles, labels = [], []
    for ax in axes[0]:
        for h, l in zip(*ax.get_legend_handles_labels()):
            if l not in labels:
                handles.append(h)
                labels.append(l)
    order = [labels.index(l) for _, l, _, _ in SERIES if l in labels]
    fig.legend([handles[i] for i in order], [labels[i] for i in order], loc="upper center",
               frameon=False, fontsize=6, ncol=1, bbox_to_anchor=(0.5, 1.0))
    fig.subplots_adjust(left=LEFT, right=RIGHT, wspace=WSPACE,
                        bottom=below / fig_h, top=1.0 - above / fig_h)
    return fig


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
