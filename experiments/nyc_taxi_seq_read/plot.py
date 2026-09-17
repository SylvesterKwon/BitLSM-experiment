#!/usr/bin/env python3
"""Plot read query time distribution (boxplots) for nyc_taxi_seq_read experiment.

Grid layout: rows = selectivity (fine -> coarse), columns = c (# query
attributes). Each cell shows boxplots of per-query time_elapsed_ms across
methods.

Usage:
    python3 experiments/nyc_taxi_seq_read/plot.py <result_dir>
    python3 experiments/nyc_taxi_seq_read/plot.py <result_dir> -o <output_dir>
"""

import argparse
import csv
import math
import os
import re

import matplotlib.pyplot as plt
import numpy as np

plt.rcParams.update({"font.size": 6, "hatch.linewidth": 0.3})

# Selectivity bands the figure draws, keyed by the upper edge each workload
# filename carries -- so sel0.0001 is the band [1e-5, 1e-4). A result directory
# is a superset: the sweep that produced this figure also measured [1e-3, 1e-2),
# and that band stays in the directory and in the summary CSV. It is off the
# figure because the three finer bands are the ones that straddle BitLSM's
# crossover, where a predicate count buys or costs something; by [1e-3, 1e-2)
# every method is doing bulk work and the panels stop separating. Restricting
# here rather than at the call site keeps the row count -- and so the height
# below -- a property of the script.
FIGURE_BANDS = (1e-5, 1e-4, 1e-3)

# Figure height (in), chosen so one panel's plot box comes out 0.702 as tall as
# it is wide -- the shape every figure in this paper shares. tight_layout gives
# the boxes whatever the furniture leaves, so this is not a function of the row
# and column counts alone: moving FIGURE_BANDS one decade finer at the same 3x4
# kept the box width and took the height from 0.918 to 0.891 (aspect 0.681),
# because the rows' y ticks and sigma labels are not the same size. Re-measure
# by reading ax.get_position() whenever FIGURE_BANDS or the grid shape moves --
# the value is measured, never eyeballed.
FIG_HEIGHT_IN = 3.77

METHOD_ORDER = [
    "no-index",
    "si-lu_strategy_pf",
    "si-lu_strategy_im",
    "si-ck_strategy_pf",
    "si-ck_strategy_im",
    "embedded_bloom_bits10",
    "sai_il2",
    "sai_il0",
    "bitlsm_rho0.03",
    "bitlsm_rho0.01",
    "bitlsm_rho0.003",
    "bitlsm_rho0.001",
]
METHOD_LABELS = {
    "no-index": "No Index",
    "si-lu_strategy_pf": "Lazy (Post Filtering)",
    "si-lu_strategy_im": "Lazy (Intersection)",
    "si-ck_strategy_pf": "Composite (Post Filtering)",
    "si-ck_strategy_im": "Composite (Intersection)",
    "embedded_bloom_bits10": "Per-Block Filters",
    # Symmetric on the one thing that differs: how many predicates the index
    # intersects. il=2 is what Cassandra ships, il=0 lifts the cap.
    "sai_il0": "SAI (intersect all)",
    "sai_il2": "SAI (intersect top-2)",
    # rho is pinned in this figure's exp_set, so it is not a variable the
    # reader is being asked to compare; it belongs to plot_rho_sensitivity.py,
    # which sweeps it. Every arm here reads the same for that reason.
    "bitlsm_rho0.03": "BitLSM",
    "bitlsm_rho0.01": "BitLSM",
    "bitlsm_rho0.003": "BitLSM",
    "bitlsm_rho0.001": "BitLSM",
}
METHOD_COLORS = {
    "no-index": "#808080",
    "si-ck_strategy_im": "#9888B8",
    "si-ck_strategy_pf": "#9888B8",
    "si-lu_strategy_im": "#4CC850",
    "si-lu_strategy_pf": "#4CC850",
    "embedded_bloom_bits10": "#1FA8A0",
    "sai_il0": "#2F6FD0",
    "sai_il2": "#2F6FD0",
    # #E04040 is BitLSM-the-method across the paper (myrocks plot.py,
    # plot_perf.py, plot_query_strip.py). #9B1B1B is the rho family's fine end
    # and the "Single + BitLSM" variant; it only reads as BitLSM in a figure
    # that sweeps rho, which this one does not -- every arm here is labelled
    # plain "BitLSM", so every arm takes the method colour.
    "bitlsm_rho0.03": "#E04040",
    "bitlsm_rho0.01": "#E04040",
    "bitlsm_rho0.003": "#E04040",
    "bitlsm_rho0.001": "#E04040",
}
METHOD_HATCHES = {
    "si-ck_strategy_im": "xxxxxx",
    "si-lu_strategy_im": "xxxxxx",
    # sai_il0 and sai_il2 share the blue hue. The hatch goes on il=0, the arm
    # we added to be generous to the baseline; il=2 is what Cassandra ships,
    # so it keeps the plain blue and is drawn first.
    "sai_il0": "xxxxxx",
}

# Pattern: read_seq_sel{sel}_k{k}_r{r}_{method}_read_log.csv
FILE_PATTERN = re.compile(
    r"^read_seq_sel([\d.]+)_k(\d+)_r(\d+)_(.+)_read_log\.csv$"
)


def load_result_dir(result_dir):
    """Load the read CSVs from result_dir that FIGURE_BANDS covers.

    Returns {(sel, k): {method: [time_ms, ...]}}.
    """
    data = {}
    skipped = set()
    for fname in os.listdir(result_dir):
        m = FILE_PATTERN.match(fname)
        if not m:
            continue
        sel = float(m.group(1))
        k = int(m.group(2))
        method = m.group(4)

        if sel not in FIGURE_BANDS:
            skipped.add(sel)
            continue

        times = []
        path = os.path.join(result_dir, fname)
        with open(path, newline="") as f:
            reader = csv.DictReader(f)
            for row in reader:
                times.append(float(row["time_elapsed_ms"]) / 1000.0)

        key = (sel, k)
        data.setdefault(key, {})
        data[key][method] = times

    for sel in sorted(skipped):
        exp = int(round(math.log10(sel)))
        print(f"Skipped band not on the figure: "
              f"sigma in [1e{exp - 1}, 1e{exp})")
    return data


def plot_grid(data, output_dir):
    """Create a grid of boxplots: rows=selectivity, cols=c."""
    # Ascending selectivity puts the finest band in the top row, so the grid
    # reads fine -> coarse downward the way the paper's text walks it.
    all_sels = sorted(set(sel for sel, _ in data))
    all_ks = sorted(set(k for _, k in data))

    if not all_sels or not all_ks:
        print("No data to plot.")
        return

    nrows = len(all_sels)
    ncols = len(all_ks)

    def sel_label(s):
        exp = int(round(math.log10(s)))
        return rf"$\sigma \in [10^{{{exp - 1}}}, 10^{{{exp}}})$"

    # Collect all methods present across every cell for a shared legend
    all_methods = []
    for cell_data in data.values():
        for m in cell_data:
            if m not in all_methods:
                all_methods.append(m)
    all_methods = [m for m in METHOD_ORDER if m in all_methods]

    fig, axes = plt.subplots(nrows, ncols, figsize=(7, FIG_HEIGHT_IN),
                             squeeze=False)

    for ri, sel in enumerate(all_sels):
        for ci, k in enumerate(all_ks):
            ax = axes[ri][ci]
            cell_data = data.get((sel, k), {})

            # Separate no-index (drawn as hline) from boxplot methods
            noindex_times = cell_data.get("no-index")
            bp_methods = [m for m in METHOD_ORDER
                          if m in cell_data and m != "no-index"]
            if not bp_methods and noindex_times is None:
                ax.text(0.5, 0.5, "N/A",
                        ha="center", va="center", transform=ax.transAxes)
                ax.set_xticks([])
                ax.set_yticks([])
                if ri == 0:
                    ax.set_title(f"c = {k}")
                if ci == 0:
                    ax.set_ylabel(f"{sel_label(sel)}\nQuery Latency (s)")
                continue

            # no-index as horizontal dashed line (mean)
            if noindex_times:
                noindex_mean = np.mean(noindex_times)
                ax.axhline(noindex_mean, color=METHOD_COLORS["no-index"],
                           linestyle="--", linewidth=0.8)

            if bp_methods:
                bp_data = [cell_data[m] for m in bp_methods]
                colors = [METHOD_COLORS.get(m, "#CCCCCC") for m in bp_methods]
                hatches = [METHOD_HATCHES.get(m, "") for m in bp_methods]

                bp = ax.boxplot(bp_data, patch_artist=True, widths=0.6,
                                showfliers=False,
                                medianprops=dict(color="black", linewidth=0.8),
                                boxprops=dict(linewidth=0.5),
                                whiskerprops=dict(linewidth=0.5),
                                capprops=dict(linewidth=0.5))
                for patch, color, hatch in zip(bp["boxes"], colors, hatches):
                    patch.set_facecolor(color)
                    patch.set_hatch(hatch)

            # Log scale: a single cell spans ~0.4s (SAI at high selectivity)
            # to ~250s (post-filtering at low selectivity). On a linear axis
            # the fast methods collapse onto the baseline and are unreadable.
            # (ticklabel_format is incompatible with a log axis.)
            ax.set_yscale("log")
            ax.set_xticks([])
            ax.tick_params(axis="x", length=0)
            ax.tick_params(axis="y", length=2, width=0.3, direction="in")
            # tick_params defaults to which="major"; on the log axis the decade
            # subdivisions are minor ticks and would otherwise point outward.
            ax.tick_params(axis="y", which="minor", length=1, width=0.3,
                           direction="in")
            ax.grid(False)

            if ri == 0:
                ax.set_title(f"c = {k}")
            if ci == 0:
                ax.set_ylabel(f"{sel_label(sel)}\nQuery Latency (s)")

    # Shared legend at top
    from matplotlib.patches import Patch
    from matplotlib.lines import Line2D
    legend_handles = []
    for m in all_methods:
        if m == "no-index":
            legend_handles.append(
                Line2D([0], [0], color=METHOD_COLORS["no-index"],
                       linestyle="--", linewidth=0.8,
                       label=METHOD_LABELS.get(m, m)))
        else:
            legend_handles.append(
                Patch(facecolor=METHOD_COLORS.get(m, "#CCCCCC"),
                      hatch=METHOD_HATCHES.get(m, ""),
                      edgecolor="black", linewidth=0.5,
                      label=METHOD_LABELS.get(m, m)))
    legend_ncol = math.ceil(len(all_methods) / 2)
    fig.tight_layout(rect=[0, 0, 1, 0.93])
    fig.legend(handles=legend_handles, loc="center",
               bbox_to_anchor=(0.5, 0.958),
               ncol=legend_ncol, frameon=False)

    out_path = os.path.join(output_dir, "read_query_time_distribution.pdf")
    fig.savefig(out_path, dpi=150)
    print(f"Saved: {out_path}")
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(
        description="Plot read query time distribution for nyc_taxi_seq_read"
    )
    parser.add_argument("result_dir", help="Path to result directory")
    parser.add_argument("-o", "--output-dir", default=None,
                        help="Output directory (default: same as result_dir)")
    args = parser.parse_args()

    output_dir = args.output_dir or args.result_dir
    os.makedirs(output_dir, exist_ok=True)

    data = load_result_dir(args.result_dir)
    if not data:
        print("No read log CSVs found.")
        return

    plot_grid(data, output_dir)


if __name__ == "__main__":
    main()
