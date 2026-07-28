#!/usr/bin/env python3
"""Plot mean query latency vs block-cache budget for nyc_taxi_read_cache_budget.

One subplot per k (# query attributes); x = block_cache budget (MB, log scale),
y = mean query latency (s). One line per method. Reads the per-budget subdirs
(mb8192/, mb4096/, ..., default/) the runner writes.

Usage:
    python3 experiments/nyc_taxi_read_cache_budget/plot.py <result_dir>
    python3 experiments/nyc_taxi_read_cache_budget/plot.py <result_dir> -o <out>
"""

import argparse
import csv
import os
import re

import matplotlib.pyplot as plt
import numpy as np

plt.rcParams.update({"font.size": 7})

# Fixed order + labels (paper convention: Lazy/Composite/Eager, strategy in
# parens, BitLSM by rho desc). Extends nyc_taxi_seq_read/plot.py with si-eager,
# sai, and rho0.01.
METHOD_ORDER = [
    "no-index",
    "si-lu_strategy_pf",
    "si-lu_strategy_im",
    "si-ck_strategy_pf",
    "si-ck_strategy_im",
    "embedded_bloom_bits10",
    "sai_il2",
    "bitlsm_rho0.01",
]
METHOD_LABELS = {
    "no-index": "No Index",
    "si-lu_strategy_pf": "Lazy (Post Filtering)",
    "si-lu_strategy_im": "Lazy (Intersection)",
    "si-ck_strategy_pf": "Composite (Post Filtering)",
    "si-ck_strategy_im": "Composite (Intersection)",
    "embedded_bloom_bits10": "Bloom + Zone Map",
    "sai_il2": "SAI",
    "bitlsm_rho0.01": r"BitLSM ($\rho$=0.01)",
}
METHOD_COLORS = {
    "no-index": "#808080",
    "si-ck_strategy_im": "#9888B8",
    "si-ck_strategy_pf": "#9888B8",
    "si-lu_strategy_im": "#4CC850",
    "si-lu_strategy_pf": "#4CC850",
    "embedded_bloom_bits10": "#1FA8A0",
    "sai_il2": "#3060C0",
    "bitlsm_rho0.01": "#9B1B1B",
}
# Intersection strategy dashed to separate from post-filtering of same color.
METHOD_LINESTYLE = {
    "si-ck_strategy_im": "--",
    "si-lu_strategy_im": "--",
}

BUDGET_DIR = re.compile(r"^mb(\d+)$")
FILE_PATTERN = re.compile(
    r"^read_seq_sel([\d.]+)_k(\d+)_r(\d+)_(.+)_read_log\.csv$"
)


def mean_latency_s(path):
    times = []
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            times.append(float(row["time_elapsed_ms"]) / 1000.0)
    return float(np.mean(times)) if times else None


def load_result_dir(result_dir):
    """Return {k: {method: {budget_mb: mean_latency_s}}}.

    budget_mb is an int; the 'default' subdir maps to 0.
    """
    data = {}
    for entry in os.listdir(result_dir):
        sub = os.path.join(result_dir, entry)
        if not os.path.isdir(sub):
            continue
        m = BUDGET_DIR.match(entry)
        if m:
            budget = int(m.group(1))
        elif entry == "default":
            budget = 0
        else:
            continue
        for fname in os.listdir(sub):
            fm = FILE_PATTERN.match(fname)
            if not fm:
                continue
            k = int(fm.group(2))
            method = fm.group(4)
            mean = mean_latency_s(os.path.join(sub, fname))
            if mean is None:
                continue
            data.setdefault(k, {}).setdefault(method, {})[budget] = mean
    return data


def plot_budget(data, output_dir):
    all_ks = sorted(data)
    if not all_ks:
        print("No data to plot.")
        return

    ncols = len(all_ks)
    fig, axes = plt.subplots(1, ncols, figsize=(3.2 * ncols, 3.0),
                             squeeze=False)

    present = []
    for k in all_ks:
        for method in data[k]:
            if method not in present:
                present.append(method)
    ordered = [m for m in METHOD_ORDER if m in present]
    ordered += [m for m in present if m not in METHOD_ORDER]  # unknowns last

    for ci, k in enumerate(all_ks):
        ax = axes[0][ci]
        for method in ordered:
            series = data[k].get(method)
            if not series:
                continue
            budgets = sorted(b for b in series if b > 0)  # budget sweep only
            if not budgets:
                continue
            ys = [series[b] for b in budgets]
            ax.plot(budgets, ys,
                    marker="o", markersize=3, linewidth=1.0,
                    color=METHOD_COLORS.get(method, "#333333"),
                    linestyle=METHOD_LINESTYLE.get(method, "-"),
                    label=METHOD_LABELS.get(method, method))
        ax.set_xscale("log", base=2)
        ax.set_xlabel("block_cache budget (MB)")
        if ci == 0:
            ax.set_ylabel("Mean query latency (s)")
        ax.set_title(f"c = {k}")
        ax.grid(True, which="both", linewidth=0.3, alpha=0.4)

    handles, labels = axes[0][0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center",
               ncol=min(len(labels), 5), frameon=False, fontsize=6)
    fig.tight_layout(rect=[0, 0, 1, 0.88])

    out_path = os.path.join(output_dir, "read_cache_budget.pdf")
    fig.savefig(out_path, dpi=150)
    print(f"Saved: {out_path}")
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(
        description="Plot latency vs block-cache budget"
    )
    parser.add_argument("result_dir", help="Path to result directory")
    parser.add_argument("-o", "--output-dir", default=None,
                        help="Output directory (default: same as result_dir)")
    args = parser.parse_args()

    output_dir = args.output_dir or args.result_dir
    os.makedirs(output_dir, exist_ok=True)

    data = load_result_dir(args.result_dir)
    plot_budget(data, output_dir)


if __name__ == "__main__":
    main()
