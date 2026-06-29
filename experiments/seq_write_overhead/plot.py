#!/usr/bin/env python3
"""Plot CPU/memory overhead for seq_write_overhead experiment.

Reads honk_player resource_monitor logs from a result directory and plots:
  - RSS (GB) vs records written
  - Cumulative process CPU time (sec) vs records written
  - Final CPU time per method, stacked by thread group

Usage:
    python3 experiments/seq_write_overhead/plot.py <result_dir>
"""

import argparse
import csv
import glob
import os
import re

import matplotlib.pyplot as plt
import numpy as np

plt.rcParams.update({"font.size": 6})


METHOD_ORDER = [
    "no-index",
    "si-lu",
    "si-ck",
    "embedded_bloom_bits10",
    "bitlsm_rho0.2",
    "bitlsm_rho0.1",
    "bitlsm_rho0.05",
]
METHOD_LABELS = {
    "no-index": "No Index",
    "si-lu": "Lazy",
    "si-ck": "Composite",
    "embedded_bloom_bits10": "Bloom + Zone Map",
    "bitlsm_rho0.2": r"BitLSM ($\rho$=0.2)",
    "bitlsm_rho0.1": r"BitLSM ($\rho$=0.1)",
    "bitlsm_rho0.05": r"BitLSM ($\rho$=0.05)",
}
METHOD_COLORS = {
    "bitlsm_rho0.2": "#F08C7C",
    "bitlsm_rho0.1": "#E04040",
    "bitlsm_rho0.05": "#9B1B1B",
    "si-lu": "#4CC850",
    "si-ck": "#9888B8",
    "embedded_bloom_bits10": "#1FA8A0",
    "no-index": "#5A5A5A",
}
METHOD_MARKERS = {
    "no-index": "o",
    "si-ck": "^",
    "si-lu": "X",
    "embedded_bloom_bits10": "P",
    "bitlsm_rho0.2": "s",
    "bitlsm_rho0.1": "D",
    "bitlsm_rho0.05": "v",
}

THREAD_GROUPS = ["writer", "rocksdb:high", "rocksdb:low"]
THREAD_GROUP_COLORS = {
    "writer": "#E69F00",
    "rocksdb:high": "#56B4E9",
    "rocksdb:low": "#0072B2",
}
THREAD_GROUP_LABELS = {
    "writer": "Foreground",
    "rocksdb:high": "Background (flush)",
    "rocksdb:low": "Background (compaction)",
    "other": "Other",
}

CLK_TCK = os.sysconf("SC_CLK_TCK")

METHOD_PATTERN = re.compile(
    r".*?_(no-index|si-ck|si-lu|si-eager|bitlsm(?:_rho[\d.]+)?)"
    r"(?:_strategy_(?:im|pf))?"
    r"_(?:sample|thread)_log\.csv$"
)


def method_from_filename(fname: str) -> str | None:
    m = METHOD_PATTERN.match(fname)
    return m.group(1) if m else None


def get_ordered_methods(keys):
    ordered = [m for m in METHOD_ORDER if m in keys]
    remaining = sorted(set(keys) - set(METHOD_ORDER))
    return ordered + remaining


def label_for(method):
    return METHOD_LABELS.get(method, method)


def color_for(method):
    return METHOD_COLORS.get(method)


def marker_for(method):
    return METHOD_MARKERS.get(method, "o")


def load_sample_logs(result_dir: str):
    """Return {method: [(records, rss_kb), ...]} sorted by records."""
    data: dict[str, list[tuple[int, int]]] = {}
    for fpath in sorted(glob.glob(os.path.join(result_dir, "*_sample_log.csv"))):
        method = method_from_filename(os.path.basename(fpath))
        if not method:
            continue
        with open(fpath, newline="") as f:
            reader = csv.DictReader(f)
            points = [(int(r["insertions"]), int(r["rss_kb"])) for r in reader]
        points.sort(key=lambda p: p[0])
        data[method] = points
    return data


def load_thread_logs(result_dir: str):
    """Return {method: {records: {group: cpu_ticks_sum}}}.

    cpu_ticks_sum aggregates utime+stime across all threads in the group at
    that checkpoint. Threads not in THREAD_GROUPS are bucketed under "other".
    """
    data: dict[str, dict[int, dict[str, int]]] = {}
    for fpath in sorted(glob.glob(os.path.join(result_dir, "*_thread_log.csv"))):
        method = method_from_filename(os.path.basename(fpath))
        if not method:
            continue
        per_records: dict[int, dict[str, int]] = {}
        with open(fpath, newline="") as f:
            for r in csv.DictReader(f):
                records = int(r["insertions"])
                comm = r["comm"]
                group = comm if comm in THREAD_GROUPS else "other"
                ticks = int(r["utime_ticks"]) + int(r["stime_ticks"])
                bucket = per_records.setdefault(records, {})
                bucket[group] = bucket.get(group, 0) + ticks
        data[method] = per_records
    return data


def plot_rss(sample_data, output_dir):
    methods = get_ordered_methods(sample_data.keys())

    fig, ax = plt.subplots(figsize=(3.333, 3.333 * 0.618))

    for method in methods:
        points = sample_data[method]
        if not points:
            continue
        records_m = np.array([p[0] for p in points]) / 1e6
        rss_gb = np.array([p[1] for p in points]) / (1024 * 1024)
        color = color_for(method)
        ax.plot(records_m, rss_gb, marker=marker_for(method), markersize=2.5,
                markevery=max(1, len(records_m) // 12),
                linewidth=0.8, label=label_for(method),
                **({"color": color} if color else {}))

    ax.set_xlabel("Records Written (M)")
    ax.set_ylabel("RSS (GB)")
    ax.set_xlim(left=0)
    ax.set_ylim(bottom=0)
    ax.tick_params(axis="x", length=2, width=0.3, direction="in")
    ax.tick_params(axis="y", length=2, width=0.3, direction="in")
    ax.legend(loc="upper right", frameon=False, ncol=2)
    ax.grid(False)
    fig.tight_layout()

    out_path = os.path.join(output_dir, "rss_over_progress.pdf")
    fig.savefig(out_path, dpi=150, bbox_inches="tight")
    print(f"Saved: {out_path}")
    plt.close(fig)


def plot_cpu(thread_data, output_dir):
    methods = get_ordered_methods(thread_data.keys())

    fig, ax = plt.subplots(figsize=(3.333, 3.333 * 0.618))

    for method in methods:
        per_records = thread_data[method]
        if not per_records:
            continue
        records = sorted(per_records.keys())
        cpu_sec = [sum(per_records[r].values()) / CLK_TCK for r in records]
        records_m = np.array(records) / 1e6
        color = color_for(method)
        ax.plot(records_m, cpu_sec, marker=marker_for(method), markersize=2.5,
                markevery=max(1, len(records_m) // 12),
                linewidth=0.8, label=label_for(method),
                **({"color": color} if color else {}))

    ax.set_xlabel("Records Written (M)")
    ax.set_ylabel("Cumulative CPU Time (s)")
    ax.set_xlim(left=0)
    ax.set_ylim(bottom=0)
    ax.tick_params(axis="x", length=2, width=0.3, direction="in")
    ax.tick_params(axis="y", length=2, width=0.3, direction="in")
    ax.legend(loc="upper right", frameon=False, ncol=2)
    ax.grid(False)
    fig.tight_layout()

    out_path = os.path.join(output_dir, "cpu_over_progress.pdf")
    fig.savefig(out_path, dpi=150, bbox_inches="tight")
    print(f"Saved: {out_path}")
    plt.close(fig)


def plot_cpu_breakdown(thread_data, output_dir):
    methods = get_ordered_methods(thread_data.keys())

    final_per_method: dict[str, dict[str, float]] = {}
    for method in methods:
        per_records = thread_data[method]
        if not per_records:
            continue
        last_records = max(per_records.keys())
        groups = per_records[last_records]
        final_per_method[method] = {
            g: groups.get(g, 0) / CLK_TCK for g in THREAD_GROUPS
        }
        other = sum(v for k, v in groups.items() if k not in THREAD_GROUPS)
        if other > 0:
            final_per_method[method]["other"] = other / CLK_TCK

    if not final_per_method:
        print("Skipping cpu_breakdown: no thread log data.")
        return

    methods = list(final_per_method.keys())
    x = np.arange(len(methods))
    fig, ax = plt.subplots(figsize=(3.333, 3.333 * 0.618))

    bottoms = np.zeros(len(methods))
    groups_present = list(THREAD_GROUPS)
    if any("other" in v for v in final_per_method.values()):
        groups_present.append("other")

    for group in groups_present:
        vals = np.array([final_per_method[m].get(group, 0) for m in methods])
        color = THREAD_GROUP_COLORS.get(group, "#999999")
        ax.bar(x, vals, bottom=bottoms, color=color,
               label=THREAD_GROUP_LABELS.get(group, group), width=0.6,
               edgecolor="white", linewidth=0.3)
        bottoms += vals

    y_max = bottoms.max() if len(bottoms) else 0
    offset = y_max * 0.01
    for xi, total in zip(x, bottoms):
        ax.text(xi, total + offset, f"{total:.0f}s",
                ha="center", va="bottom", fontsize=5)

    ax.set_xticks(x)
    ax.set_xticklabels([label_for(m) for m in methods], rotation=30, ha="right")
    ax.set_ylabel("Total CPU Time (s)")
    ax.set_ylim(top=y_max * 1.12)
    ax.tick_params(axis="x", length=0)
    ax.tick_params(axis="y", length=2, width=0.3, direction="in")
    ax.legend(loc="upper right", frameon=False)
    ax.grid(False)
    fig.tight_layout()

    out_path = os.path.join(output_dir, "cpu_breakdown.pdf")
    fig.savefig(out_path, dpi=150, bbox_inches="tight")
    print(f"Saved: {out_path}")
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(
        description="Plot CPU/memory overhead for seq_write_overhead experiment"
    )
    parser.add_argument("result_dir", help="Result directory containing *_sample_log.csv "
                                           "and *_thread_log.csv files")
    parser.add_argument("-o", "--output-dir", default=None,
                        help="Output directory (default: same as result_dir)")
    args = parser.parse_args()

    output_dir = args.output_dir or args.result_dir
    os.makedirs(output_dir, exist_ok=True)

    sample_data = load_sample_logs(args.result_dir)
    thread_data = load_thread_logs(args.result_dir)

    if not sample_data and not thread_data:
        print(f"No sample/thread logs found under {args.result_dir}")
        return

    if sample_data:
        plot_rss(sample_data, output_dir)
    if thread_data:
        plot_cpu(thread_data, output_dir)
        plot_cpu_breakdown(thread_data, output_dir)


if __name__ == "__main__":
    main()
