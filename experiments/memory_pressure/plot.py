#!/usr/bin/env python3
"""Plot median query latency vs block-cache budget for memory_pressure.

One subplot per k (# query attributes); x = block_cache budget (MB, log scale),
y = mean query latency (s). One line per method. Reads the per-budget subdirs
(mb8192/, mb4096/, ..., default/) the runner writes.

Usage:
    python3 experiments/memory_pressure/plot.py <result_dir>
    python3 experiments/memory_pressure/plot.py <result_dir> -o <out>
"""

import argparse
import csv
import os
import re

import matplotlib.pyplot as plt
import numpy as np

# Height/width of one panel's plot box. Measured from nyc_taxi_seq_read's
# 4x4 grid, whose panels come out 1.294 x 0.909 in.
PANEL_BOX_ASPECT = 0.702

plt.rcParams.update({
    "font.size": 6,
    # Ticks point into the axes, short and hairline-thin, matching
    # seq_write_wa/plot.py.
    "xtick.direction": "in", "ytick.direction": "in",
    "xtick.major.size": 2, "ytick.major.size": 2,
    "xtick.minor.size": 1, "ytick.minor.size": 1,
    "xtick.major.width": 0.3, "ytick.major.width": 0.3,
    "xtick.minor.width": 0.3, "ytick.minor.width": 0.3,
})

# Fixed order + labels (paper convention: Lazy/Composite/Eager, strategy in
# parens, BitLSM by rho desc). Extends nyc_taxi_seq_read/plot.py with si-eager,
# sai, and rho0.01.
#
# The formal full-baseline sweep (exp_set/sel0.0001.json) runs
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
    "sai_il2",
    "sai_il0",
    "sai_il2_ondemand",
    "sai_il0_ondemand",
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
    "embedded_bloom_bits10_ondemand": "Bloom + Zone Map",
    "sai_il0": "SAI (intersect all)",
    "sai_il2": "SAI (intersect top-2)",
    "sai_il0_ondemand": "SAI (intersect all)",
    "sai_il2_ondemand": "SAI (intersect top-2)",
    "bitlsm_rho0.01": "BitLSM",
    "bitlsm_rho0.01_ondemand": "BitLSM",
    "bitlsm_rho0.001": "BitLSM",
    "bitlsm_rho0.001_ondemand": "BitLSM",
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
    "sai_il0": "--",
    "sai_il0_ondemand": "--",
}

BUDGET_DIR = re.compile(r"^mb(\d+)$")
FILE_PATTERN = re.compile(
    r"^read_seq_sel([\d.]+)_k(\d+)_r(\d+)_(.+)_read_log\.csv$"
)


def budget_tick_labels(budgets):
    """Label every other budget, blanking the rest.

    All six ticks stay drawn, so the reader still sees where the measurements
    are, but four-digit MB values every 1.4 in of panel crowd into each other.
    """
    return [str(b) if i % 2 == 0 else "" for i, b in enumerate(budgets)]


def median_latency_s(path):
    """Median per-query latency, the statistic the query section reports.

    A boxplot's centre line is a median, so the unbounded anchor here lands on
    the same value the query section shows for the same configuration, which
    is what ties the two together. It also keeps each run's cold first queries
    from setting the number. Sums cannot be re-medianed afterwards, so this
    reads the per-query rows.
    """
    times = []
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            times.append(float(row["time_elapsed_ms"]) / 1000.0)
    return float(np.median(times)) if times else None


def index_read_mb_per_query(path):
    """Index bytes read from the device, per query.

    The run total divided by its query count, not a median: these bytes are a
    cost that accumulates, and the per-query volume falls across a run as the
    cache warms, so the middle query is not a representative one. Dividing
    also keeps the axis in MB, where "this query touched 380 MB of index"
    reads as a property of the workload rather than a suspicious total.
    """
    vals = []
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            if row.get("idx_read_mb"):
                vals.append(float(row["idx_read_mb"]))
    return sum(vals) / len(vals) if vals else None


def data_read_mb_per_query(path):
    """Data-block bytes read per query: everything read that was not index.

    disk_read_mb is the process's whole read volume, so subtracting the index
    share leaves the candidate rows the query actually fetched -- the other
    half of the cost, and the control that says the latency gap is an index
    effect rather than a difference in how many rows each engine touches.
    Averaged per query for the same reason as the index figure.
    """
    vals = []
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            if row.get("disk_read_mb") and row.get("idx_read_mb"):
                vals.append(max(0.0, float(row["disk_read_mb"]) - float(row["idx_read_mb"])))
    return sum(vals) / len(vals) if vals else None


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
    """Return ({k: {method: {budget_mb: median_latency_s}}}, same shape for RSS and reads).

    budget_mb is an int; the 'default' subdir maps to 0.
    """
    data = {}
    rss = {}
    dat = {}
    idx = {}
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
            med = median_latency_s(path)
            if med is None:
                continue
            data.setdefault(k, {}).setdefault(method, {})[budget] = med
            peak = peak_rss_gb(path)
            if peak is not None:
                rss.setdefault(k, {}).setdefault(method, {})[budget] = peak
            idx.setdefault(k, {}).setdefault(method, {})[budget] = index_read_mb_per_query(path)
            dat.setdefault(k, {}).setdefault(method, {})[budget] = data_read_mb_per_query(path)
    return data, rss, idx, dat


# The three figures stack as (a) latency, (b) index reads, (c) data reads, so
# each piece of shared furniture is drawn once: column titles and the legend on
# the top panel, the x label on the bottom one. Panel geometry is pinned rather
# than left to tight_layout, because the y labels differ in length and a
# per-figure layout would misalign the columns once they are stacked.
# Left is sized so the y label starts as far from the edge as the last
# panel ends from the other one; anything larger just shifts the figure
# right inside its column.
LEFT, RIGHT, WSPACE = 0.061, 0.995, 0.11
# Each row is exactly as tall as it needs to be: the panel box, plus only the
# furniture that row carries. Two constraints make this fiddly enough to be
# worth computing rather than eyeballing.
#
# The axes area must equal the box. Leaving it taller does not enlarge the box
# (the aspect is pinned) -- it pads above and below, and that padding is the
# gap that appears under a stacked row. Leaving it shorter is worse: the box
# shrinks to fit, and because the aspect holds it narrows too, so the stacked
# columns stop lining up.
#
# And a rotated y label is centred on the box while being longer than it
# (1.05-1.16 in against a 0.92 in box), so each row needs roughly an eighth of
# an inch of figure above and below the box or the label is clipped.
FIG_W = 7.0            # two-column figure width in inches
TICKS_H = 0.14         # x tick labels under the box (also clears the y label)
YLABEL_OVERHANG = 0.13  # half the amount a rotated y label exceeds the box
LEGEND_H = 0.28        # legend strip above
TITLES_H = 0.13        # "c = N" column titles
XLABEL_H = 0.10        # shared x label under the ticks


ROLE_FURNITURE = {
    # role: (inches above the box, inches below it)
    "top": (LEGEND_H + TITLES_H, TICKS_H),
    "middle": (YLABEL_OVERHANG, TICKS_H),
    "bottom": (YLABEL_OVERHANG, TICKS_H + XLABEL_H),
    "solo": (LEGEND_H + TITLES_H, TICKS_H + XLABEL_H),
}


def row_layout(role, ncols):
    """(figure height in inches, bottom, top) for one row of panels.

    The box height follows from the width the columns get, so it is derived
    here rather than fixed: change WSPACE or the column count and the rows
    still come out the same height as their boxes.
    """
    box_w = FIG_W * (RIGHT - LEFT) / (ncols + (ncols - 1) * WSPACE)
    box_h = box_w * PANEL_BOX_ASPECT + 0.01  # a hair of slack, never less
    above, below = ROLE_FURNITURE[role]
    h = box_h + above + below
    return h, below / h, 1.0 - above / h


def draw_panels(series_by_k, ylabel, out_name, output_dir, role="solo",
                reference_line=False):
    """One row of per-c panels sharing a y axis.

    `role` says which shared furniture this figure carries when the three are
    stacked in the paper: "top" keeps the column titles and the legend,
    "bottom" keeps the x label, "middle" keeps neither, "solo" keeps all.
    """
    all_ks = sorted(series_by_k)
    if not all_ks:
        return

    ncols = len(all_ks)
    fig_h, bottom, top = row_layout(role, ncols)
    fig, axes = plt.subplots(1, ncols, figsize=(FIG_W, fig_h),
                             squeeze=False, sharey=True)

    present = []
    for k in all_ks:
        for method in series_by_k[k]:
            if method not in present:
                present.append(method)
    ordered = [m for m in METHOD_ORDER if m in present]
    ordered += [m for m in present if m not in METHOD_ORDER]

    for ci, k in enumerate(all_ks):
        ax = axes[0][ci]
        budgets = []
        for method in ordered:
            series = series_by_k[k].get(method)
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
        if reference_line and budgets:
            # y = budget: a method whose memory the budget really bounds tracks
            # this line, and on log-log it is straight.
            ax.plot(budgets, [b / 1024.0 for b in budgets],
                    color="#999999", linewidth=0.6, linestyle=":")
        ax.set_xscale("log", base=2)
        ax.set_yscale("log")
        # Plain MB numbers read faster than 2^n; every other one is labelled so
        # four-digit values stop colliding on a narrow panel.
        ax.set_xticks(budgets)
        ax.set_xticklabels(budget_tick_labels(budgets))
        ax.tick_params(axis="x", which="minor", length=0)
        if ci == 0:
            ax.set_ylabel(ylabel)
        ax.set_box_aspect(PANEL_BOX_ASPECT)
        if role in ("top", "solo"):
            ax.set_title(f"c = {k}")
        ax.grid(False)

    if role in ("bottom", "solo"):
        # Sits just under the tick labels; the default drops it to the very
        # bottom of the figure, leaving a visible gap when rows are stacked.
        fig.supxlabel("block cache budget (MB)", fontsize=6, y=0.015)
    if role in ("top", "solo"):
        handles, labels = axes[0][0].get_legend_handles_labels()
        fig.legend(handles, labels, loc="upper center",
                   ncol=min(len(labels), 5), frameon=False, fontsize=6)
    fig.subplots_adjust(left=LEFT, right=RIGHT, bottom=bottom, top=top,
                        wspace=WSPACE)

    out_path = os.path.join(output_dir, out_name)
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

    data, rss, idx, dat = load_result_dir(args.result_dir)
    draw_panels(data, "Median query latency (s)",
                "memory_pressure.pdf", output_dir, role="top")
    draw_panels(idx, "Avg. index read (MB/query)",
                "memory_pressure_index_reads.pdf", output_dir, role="middle")
    draw_panels(dat, "Avg. data read (MB/query)",
                "memory_pressure_data_reads.pdf", output_dir, role="bottom")
    draw_panels(rss, "Peak RSS (GB)",
                "memory_pressure_rss.pdf", output_dir, role="solo",
                reference_line=True)


if __name__ == "__main__":
    main()
