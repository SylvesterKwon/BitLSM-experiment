#!/usr/bin/env python3
"""Plot read latency for the ULID key-correlation subsection (single column).

Two panels: queries with at least one predicate on a time-correlated attribute
(tpep_pickup_datetime, which the ULID key carries, or tpep_dropoff_datetime)
and queries without one. Each panel holds one cluster of per-query latency
boxplots per predicate count c.

The clusters are not a series. Every c is its own workload -- different
attribute combinations, and each range predicate gets a different share of the
selectivity budget -- so nothing connects them and methods are compared only
within a cluster.

Boxes follow nyc_taxi_seq_read/plot.py: per-query latency in seconds on a log
axis, median line, no fliers, and the same-hue variant hatched (BitLSM with
global bins beside BitLSM).

Input is key_correlation_queries.csv from summarize_key_correlation.py.

Usage:
    python3 experiments/nyc_taxi_seq_read/plot_key_correlation.py <read_result_dir>
    python3 experiments/nyc_taxi_seq_read/plot_key_correlation.py <read_result_dir> -o <output_dir>
"""

import argparse
import csv
import os
from collections import defaultdict

import matplotlib.pyplot as plt
import matplotlib.ticker as mticker
from matplotlib.patches import Patch

plt.rcParams.update({
    "font.size": 6,
    "hatch.linewidth": 0.3,
    # Ticks point into the axes, short and hairline-thin, matching
    # seq_write_wa/plot.py.
    "xtick.direction": "in", "ytick.direction": "in",
    "xtick.major.size": 2, "ytick.major.size": 2,
    "xtick.minor.size": 1, "ytick.minor.size": 1,
    "xtick.major.width": 0.3, "ytick.major.width": 0.3,
    "xtick.minor.width": 0.3, "ytick.minor.width": 0.3,
})

# Height/width of one panel's plot box, measured from nyc_taxi_seq_read's 4x4
# grid (1.294 x 0.909 in). A single row does not reach it on its own, so it is
# pinned per axis.
PANEL_BOX_ASPECT = 0.702
FIG_W = 3.333               # single text column, inches
# Geometry is pinned rather than left to tight_layout. With the aspect fixed,
# any figure height beyond box + furniture is only margin, so the height is
# computed from the box.
LEFT, RIGHT, WSPACE = 0.125, 0.99, 0.08
LEGEND_H = 0.20             # legend strip above the titles
TITLES_H = 0.14             # panel titles
TICKS_H = 0.13              # "c = N" tick labels under the box

# (method key as in the result file names, legend label, color, hatch).
# Colors and the hatch-for-variant rule come from nyc_taxi_seq_read/plot.py.
METHODS = [
    ("bitlsm_rho0.001", "BitLSM", "#9B1B1B", ""),
    ("bitlsm-global_rho0.001", "BitLSM (global bins)", "#9B1B1B", "xxxxxx"),
    ("embedded_bloom_bits10", "Bloom + Zone Map", "#1FA8A0", ""),
]
PANELS = [
    ("True", "Time-correlated queries"),
    ("False", "Uncorrelated queries"),
]
BOX_W = 0.8
CLUSTER_STEP = len(METHODS) + 1     # one box-width of gap between clusters
QUERIES_CSV = "key_correlation_queries.csv"


def load_latencies(path):
    """{(time_correlated, k, method): [latency_s, ...]}."""
    lat = defaultdict(list)
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            lat[(r["time_correlated"], int(r["k"]), r["method"])].append(
                float(r["time_elapsed_ms"]) / 1000.0)
    return lat


def plot(lat, out_path):
    ks = sorted({k for (_, k, _) in lat})
    box_w = FIG_W * (RIGHT - LEFT) / (len(PANELS) + (len(PANELS) - 1) * WSPACE)
    box_h = box_w * PANEL_BOX_ASPECT + 0.01   # a hair of slack, never less
    above, below = LEGEND_H + TITLES_H, TICKS_H
    fig_h = above + box_h + below
    fig, axes = plt.subplots(1, len(PANELS), figsize=(FIG_W, fig_h),
                             sharey=True)

    for ax, (group, title) in zip(axes, PANELS):
        for ci, k in enumerate(ks):
            center = ci * CLUSTER_STEP
            for mi, (key, _, color, hatch) in enumerate(METHODS):
                values = lat.get((group, k, key))
                if not values:
                    continue
                offset = mi - (len(METHODS) - 1) / 2
                bp = ax.boxplot([values], positions=[center + offset],
                                widths=BOX_W, patch_artist=True,
                                showfliers=False,
                                medianprops=dict(color="black", linewidth=0.8),
                                boxprops=dict(linewidth=0.5),
                                whiskerprops=dict(linewidth=0.5),
                                capprops=dict(linewidth=0.5))
                for patch in bp["boxes"]:
                    patch.set_facecolor(color)
                    patch.set_hatch(hatch)
        ax.set_xticks([ci * CLUSTER_STEP for ci in range(len(ks))])
        ax.set_xticklabels([f"c = {k}" for k in ks])
        ax.tick_params(axis="x", length=0)
        half = CLUSTER_STEP / 2
        ax.set_xlim(-half, (len(ks) - 1) * CLUSTER_STEP + half)
        # Latency spans milliseconds (BitLSM) to minutes (Bloom + Zone Map on
        # uncorrelated predicates); on a linear axis the fast boxes vanish.
        ax.set_yscale("log")
        ax.yaxis.set_major_formatter(
            mticker.FuncFormatter(lambda v, _: f"{v:g}"))
        ax.yaxis.set_minor_formatter(mticker.NullFormatter())
        ax.set_box_aspect(PANEL_BOX_ASPECT)
        ax.set_title(title, fontsize=6)
        ax.grid(False)
    axes[0].set_ylabel("Query latency (s)")

    handles = [Patch(facecolor=color, hatch=hatch, edgecolor="black",
                     linewidth=0.5, label=label)
               for _, label, color, hatch in METHODS]
    fig.legend(handles=handles, loc="upper center", ncol=len(handles),
               frameon=False, fontsize=6, handlelength=1.6, columnspacing=1.4)
    fig.subplots_adjust(left=LEFT, right=RIGHT, bottom=below / fig_h,
                        top=1 - above / fig_h, wspace=WSPACE)
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"Saved: {out_path}")


def main():
    parser = argparse.ArgumentParser(
        description="Plot ULID key-correlation read latency")
    parser.add_argument("result_dir",
                        help=f"Read result directory holding {QUERIES_CSV}")
    parser.add_argument("-o", "--output-dir", default=None,
                        help="Output directory (default: result_dir)")
    args = parser.parse_args()

    queries = os.path.join(args.result_dir, QUERIES_CSV)
    if not os.path.exists(queries):
        raise SystemExit(f"{queries} not found; run "
                         "summarize_key_correlation.py first")
    output_dir = args.output_dir or args.result_dir
    os.makedirs(output_dir, exist_ok=True)
    plot(load_latencies(queries),
         os.path.join(output_dir, "key_correlation_latency.pdf"))


if __name__ == "__main__":
    main()
