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
#
# The formal full-baseline sweep (read_cache_budget_sel0.0001.json) runs
# bitlsm/sai/embedded with --index_mode ondemand; honk_player's ParamSuffix()
# appends "_ondemand" to those CSV names (si-ck/si-lu never carry it — they
# have no --index_mode flag). The "_ondemand" entries below are additive:
# resident-mode keys (bitlsm_rho0.01, sai_il2, embedded_bloom_bits10) are kept
# for old result dirs and never appear in the same result dir as their
# ondemand counterparts.
METHOD_ORDER = [
    "no-index",
    "si-lu_strategy_pf",
    "si-lu_strategy_im",
    "si-ck_strategy_pf",
    "si-ck_strategy_im",
    "embedded_bloom_bits10",
    "embedded_bloom_bits10_ondemand",
    "sai_il0",
    "sai_il2",
    "sai_il0_ondemand",
    "sai_il2_ondemand",
    "bitlsm_rho0.01",
    "bitlsm_rho0.01_ondemand",
    "bitlsm_rho0.001",
    "bitlsm_rho0.001_ondemand",
]
METHOD_LABELS = {
    "no-index": "No Index",
    "si-lu_strategy_pf": "Lazy (Post Filtering)",
    "si-lu_strategy_im": "Lazy (Intersection)",
    "si-ck_strategy_pf": "Composite (Post Filtering)",
    "si-ck_strategy_im": "Composite (Intersection)",
    "embedded_bloom_bits10": "Bloom + Zone Map",
    "embedded_bloom_bits10_ondemand": "Bloom + Zone Map (OD)",
    "sai_il0": "SAI (il=0)",
    "sai_il2": "SAI",
    "sai_il0_ondemand": "SAI (il=0, OD)",
    "sai_il2_ondemand": "SAI (il=2, OD)",
    "bitlsm_rho0.01": r"BitLSM ($\rho$=0.01)",
    "bitlsm_rho0.01_ondemand": r"BitLSM ($\rho$=0.01, OD)",
    "bitlsm_rho0.001": r"BitLSM ($\rho$=0.001)",
    "bitlsm_rho0.001_ondemand": r"BitLSM ($\rho$=0.001, OD)",
}
METHOD_COLORS = {
    "no-index": "#808080",
    "si-ck_strategy_im": "#9888B8",
    "si-ck_strategy_pf": "#9888B8",
    "si-lu_strategy_im": "#4CC850",
    "si-lu_strategy_pf": "#4CC850",
    "embedded_bloom_bits10": "#1FA8A0",
    "embedded_bloom_bits10_ondemand": "#1FA8A0",
    "sai_il0": "#3060C0",
    "sai_il2": "#3060C0",
    "sai_il0_ondemand": "#3060C0",
    "sai_il2_ondemand": "#3060C0",
    "bitlsm_rho0.01": "#9B1B1B",
    "bitlsm_rho0.01_ondemand": "#9B1B1B",
    "bitlsm_rho0.001": "#D9534F",
    "bitlsm_rho0.001_ondemand": "#D9534F",
}
# Intersection strategy dashed to separate from post-filtering of same color.
# sai_il2_ondemand (Cassandra default) is dashed to separate it from
# sai_il0_ondemand (the fair, unlimited-intersection comparison) at the same
# blue hue.
METHOD_LINESTYLE = {
    "si-ck_strategy_im": "--",
    "si-lu_strategy_im": "--",
    "sai_il2": "--",
    "sai_il2_ondemand": "--",
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


def peak_rss_gb(path):
    """Highest peak RSS the run reached, in GB.

    The budget caps the block cache, not the process, so this is what shows
    whether a nominal budget actually bounds resident memory.
    """
    peaks = []
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            if row.get("peak_rss_kb"):
                peaks.append(float(row["peak_rss_kb"]) / 1048576.0)
    return max(peaks) if peaks else None


def load_result_dir(result_dir):
    """Return ({k: {method: {budget_mb: mean_latency_s}}}, same shape for RSS).

    budget_mb is an int; the 'default' subdir maps to 0.
    """
    data = {}
    rss = {}
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
            path = os.path.join(sub, fname)
            mean = mean_latency_s(path)
            if mean is None:
                continue
            data.setdefault(k, {}).setdefault(method, {})[budget] = mean
            peak = peak_rss_gb(path)
            if peak is not None:
                rss.setdefault(k, {}).setdefault(method, {})[budget] = peak
    return data, rss


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


def plot_rss(rss, output_dir):
    """Peak RSS vs budget: does the budget actually bound resident memory?

    The budget caps one LRU cache, so a method whose readers live outside it
    would track the y = x line poorly (or not at all). The diagonal is drawn
    for reference; points below it sit within their nominal budget.
    """
    all_ks = sorted(rss)
    if not all_ks:
        return

    ncols = len(all_ks)
    fig, axes = plt.subplots(1, ncols, figsize=(3.2 * ncols, 3.0),
                             squeeze=False)

    present = []
    for k in all_ks:
        for method in rss[k]:
            if method not in present:
                present.append(method)
    ordered = [m for m in METHOD_ORDER if m in present]
    ordered += [m for m in present if m not in METHOD_ORDER]

    for ci, k in enumerate(all_ks):
        ax = axes[0][ci]
        for method in ordered:
            series = rss[k].get(method)
            if not series:
                continue
            budgets = sorted(b for b in series if b > 0)
            if not budgets:
                continue
            ax.plot(budgets, [series[b] for b in budgets],
                    marker="o", markersize=3, linewidth=1.0,
                    color=METHOD_COLORS.get(method, "#333333"),
                    linestyle=METHOD_LINESTYLE.get(method, "-"),
                    label=METHOD_LABELS.get(method, method))
            ax.plot(budgets, [b / 1024.0 for b in budgets],
                    color="#999999", linewidth=0.6, linestyle=":")
        ax.set_xscale("log", base=2)
        ax.set_xlabel("block_cache budget (MB)")
        if ci == 0:
            ax.set_ylabel("Peak RSS (GB)")
        ax.set_title(f"c = {k}")
        ax.grid(True, which="both", linewidth=0.3, alpha=0.4)

    handles, labels = axes[0][0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center",
               ncol=min(len(labels), 5), frameon=False, fontsize=6)
    fig.tight_layout(rect=[0, 0, 1, 0.88])

    out_path = os.path.join(output_dir, "read_cache_budget_rss.pdf")
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

    data, rss = load_result_dir(args.result_dir)
    plot_budget(data, output_dir)
    plot_rss(rss, output_dir)


if __name__ == "__main__":
    main()
