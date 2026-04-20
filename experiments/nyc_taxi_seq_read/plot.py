#!/usr/bin/env python3
"""Plot read query time distribution (boxplots) for nyc_taxi_seq_read experiment.

Grid layout: rows = selectivity, columns = k (# query attributes).
Each cell shows boxplots of per-query time_elapsed_ms across methods.

Usage:
    python3 experiments/nyc_taxi_seq_read/plot.py <result_dir>
    python3 experiments/nyc_taxi_seq_read/plot.py <result_dir> -o <output_dir>
"""

import argparse
import csv
import os
import re

import matplotlib.pyplot as plt
import numpy as np

plt.rcParams.update({"font.size": 6})

METHOD_ORDER = [
    "no-index",
    "si-ck_strategy_im",
    "si-ck_strategy_pf",
    "si-lu_strategy_im",
    "si-lu_strategy_pf",
    "bitlsm_rho0.2",
    "bitlsm_rho0.1",
    "bitlsm_rho0.05",
]
METHOD_LABELS = {
    "no-index": "No Index",
    "si-ck_strategy_im": "CK-IM",
    "si-ck_strategy_pf": "CK-PF",
    "si-lu_strategy_im": "LU-IM",
    "si-lu_strategy_pf": "LU-PF",
    "bitlsm_rho0.2": r"bitlsm ($\rho$=0.2)",
    "bitlsm_rho0.1": r"bitlsm ($\rho$=0.1)",
    "bitlsm_rho0.05": r"bitlsm ($\rho$=0.05)",
}
METHOD_COLORS = {
    "no-index": "#808080",
    "si-ck_strategy_im": "#B8A8D8",
    "si-ck_strategy_pf": "#9888B8",
    "si-lu_strategy_im": "#7DE880",
    "si-lu_strategy_pf": "#4CC850",
    "bitlsm_rho0.2": "#F08C7C",
    "bitlsm_rho0.1": "#E04040",
    "bitlsm_rho0.05": "#9B1B1B",
}

# Pattern: read_seq_sel{sel}_k{k}_r{r}_{method}_read_log.csv
FILE_PATTERN = re.compile(
    r"^read_seq_sel([\d.]+)_k(\d+)_r(\d+)_(.+)_read_log\.csv$"
)


def load_result_dir(result_dir):
    """Load all read CSVs from result_dir.

    Returns {(sel, k): {method: [time_ms, ...]}}.
    """
    data = {}
    for fname in os.listdir(result_dir):
        m = FILE_PATTERN.match(fname)
        if not m:
            continue
        sel = float(m.group(1))
        k = int(m.group(2))
        method = m.group(4)

        times = []
        path = os.path.join(result_dir, fname)
        with open(path, newline="") as f:
            reader = csv.DictReader(f)
            for row in reader:
                times.append(float(row["time_elapsed_ms"]))

        key = (sel, k)
        data.setdefault(key, {})
        data[key][method] = times
    return data


def plot_grid(data, output_dir):
    """Create a grid of boxplots: rows=selectivity, cols=k."""
    all_sels = sorted(set(sel for sel, _ in data))
    all_ks = sorted(set(k for _, k in data))

    if not all_sels or not all_ks:
        print("No data to plot.")
        return

    nrows = len(all_sels)
    ncols = len(all_ks)

    # Collect all methods present across every cell for a shared legend
    all_methods = []
    for cell_data in data.values():
        for m in cell_data:
            if m not in all_methods:
                all_methods.append(m)
    all_methods = [m for m in METHOD_ORDER if m in all_methods]

    cell_w = 7 / ncols
    cell_h = cell_w
    fig, axes = plt.subplots(nrows, ncols, figsize=(7, cell_h * nrows),
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
                ax.text(0.5, 0.5, "No data available",
                        ha="center", va="center", transform=ax.transAxes)
                ax.set_xticks([])
                ax.set_yticks([])
                if ri == nrows - 1:
                    ax.set_xlabel(f"k = {k}")
                if ci == 0:
                    ax.set_ylabel(f"sel = {sel}\nQuery Time (ms)")
                continue

            # no-index as horizontal dashed line (mean)
            if noindex_times:
                noindex_mean = np.mean(noindex_times)
                ax.axhline(noindex_mean, color=METHOD_COLORS["no-index"],
                           linestyle="--", linewidth=0.8)

            if bp_methods:
                bp_data = [cell_data[m] for m in bp_methods]
                colors = [METHOD_COLORS.get(m, "#CCCCCC") for m in bp_methods]

                bp = ax.boxplot(bp_data, patch_artist=True, widths=0.6,
                                showfliers=False,
                                medianprops=dict(color="black", linewidth=0.8),
                                boxprops=dict(linewidth=0.5),
                                whiskerprops=dict(linewidth=0.5),
                                capprops=dict(linewidth=0.5))
                for patch, color in zip(bp["boxes"], colors):
                    patch.set_facecolor(color)

            ax.set_xticks([])
            ax.ticklabel_format(axis="y", style="sci", scilimits=(0, 0))
            ax.yaxis.get_offset_text().set_fontsize(plt.rcParams["font.size"])
            ax.tick_params(axis="x", length=0)
            ax.tick_params(axis="y", length=2, width=0.3, direction="in")
            ax.grid(False)

            if ri == nrows - 1:
                ax.set_xlabel(f"k = {k}")
            if ci == 0:
                ax.set_ylabel(f"sel = {sel}\nQuery Time (ms)")

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
                      edgecolor="black", linewidth=0.5,
                      label=METHOD_LABELS.get(m, m)))
    fig.legend(handles=legend_handles, loc="upper center",
               ncol=len(all_methods), frameon=False)
    fig.tight_layout(rect=[0, 0, 1, 0.94])

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
