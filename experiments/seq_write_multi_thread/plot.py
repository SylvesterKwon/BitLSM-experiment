#!/usr/bin/env python3
"""Plot multi-threaded write throughput scaling.

Reads honk_player write CSV logs from a result directory and plots
throughput (records/sec) vs num_threads per method.

Usage:
    python3 experiments/seq_write_multi_thread/plot.py <result_dir>
"""

import argparse
import csv
import glob
import os
import re

import matplotlib.pyplot as plt
import numpy as np
from matplotlib.ticker import FuncFormatter

plt.rcParams.update({"font.size": 6})


METHOD_ORDER = [
    "no-index",
    "si-lu",
    "si-ck",
    "bitlsm_rho0.03",
    "bitlsm_rho0.01",
    "bitlsm_rho0.003",
]
METHOD_LABELS = {
    "no-index": "No Index",
    "si-lu": "Lazy",
    "si-ck": "Composite",
    "bitlsm_rho0.01": r"BitLSM ($\rho$=0.01)",
    "bitlsm_rho0.003": r"BitLSM ($\rho$=0.003)",
    "bitlsm_rho0.03": r"BitLSM ($\rho$=0.03)",
}
METHOD_COLORS = {
    "bitlsm_rho0.03": "#F08C7C",
    "bitlsm_rho0.01": "#E04040",
    "bitlsm_rho0.003": "#9B1B1B",
    "si-lu": "#4CC850",
    "si-ck": "#9888B8",
}
METHOD_MARKERS = {
    "no-index": "o",
    "si-ck": "^",
    "si-lu": "X",
    "bitlsm_rho0.03": "s",
    "bitlsm_rho0.01": "D",
    "bitlsm_rho0.003": "v",
}


def parse_write_logs(result_dir: str):
    """Parse write log CSVs and extract final (time, records) per (method, num_threads).

    Filenames follow honk_player convention:
        write_seq_2025_all_<binding><param_suffix>_write_log.csv

    The num_threads is encoded in the directory structure by the runner,
    but since honk_player writes CSV into a flat output_dir, we need to
    match via the summary line in stdout. Instead, we parse all write logs
    and correlate with the run.py output.

    Alternative: read the master log. For simplicity, we accept a simple
    CSV that the runner could produce. But honk_player only writes per-binding
    CSVs. So let's parse those and also accept a summary CSV.

    Returns: {method: {num_threads: (total_time_sec, total_records, throughput)}}
    """
    # Look for write_log CSVs
    pattern = os.path.join(result_dir, "*_write_log.csv")
    files = sorted(glob.glob(pattern))

    results = {}
    for fpath in files:
        fname = os.path.basename(fpath)
        # Extract method + params from filename
        # e.g., write_seq_2025_all_no-index_write_log.csv
        # e.g., write_seq_2025_all_bitlsm_rho0.01_write_log.csv
        m = re.match(r".*?_(no-index|si-ck|si-lu|si-eager|bitlsm.*)_write_log\.csv$", fname)
        if not m:
            continue
        method = m.group(1)

        # Read last row to get final time and records
        with open(fpath, newline="") as f:
            rows = list(csv.DictReader(f))
        if not rows:
            continue
        last = rows[-1]
        time_ms = float(last["time_elapsed_ms"])
        records = int(last["records_written"])
        throughput = records / (time_ms / 1000) if time_ms > 0 else 0

        results.setdefault(method, []).append((time_ms / 1000, records, throughput))

    return results


def _read_rows(csv_path: str):
    with open(csv_path, newline="") as f:
        content = f.read().replace("\r\n", "\n").replace("\r", "\n")
    return list(csv.DictReader(content.strip().splitlines()))


def load_summary_csv(csv_paths):
    """Load final rows (with db_size_bytes) from one or more summary CSVs.

    Throughput per (method, num_threads) is averaged across CSVs.
    Returns: {method: [(num_threads, throughput), ...]}
    """
    if isinstance(csv_paths, str):
        csv_paths = [csv_paths]

    runs = {}  # {method: {num_threads: [throughputs, ...]}}
    for csv_path in csv_paths:
        for row in _read_rows(csv_path):
            db_bytes = row.get("db_size_bytes", "").strip()
            if not db_bytes:
                continue
            method = row["method"].strip()
            nt = int(row["num_threads"])
            time_ms = float(row["time_elapsed_ms"])
            records = int(row["records_written"])
            throughput = records / (time_ms / 1000) if time_ms > 0 else 0
            runs.setdefault(method, {}).setdefault(nt, []).append(throughput)

    data = {}
    for method, nt_map in runs.items():
        for nt, values in nt_map.items():
            data.setdefault(method, []).append((nt, sum(values) / len(values)))
    return data


def get_ordered_methods(data_keys):
    ordered = [m for m in METHOD_ORDER if m in data_keys]
    remaining = sorted(set(data_keys) - set(METHOD_ORDER))
    return ordered + remaining


def label_for(method):
    return METHOD_LABELS.get(method, method)


def color_for(method):
    return METHOD_COLORS.get(method, None)


def marker_for(method):
    return METHOD_MARKERS.get(method, "o")


def plot_throughput_scaling(data, output_dir):
    """Line chart: throughput (records/sec) vs num_threads per method."""
    methods = get_ordered_methods(data.keys())

    fig, ax = plt.subplots(figsize=(3.333, 2.2))
    ax.set_box_aspect(0.5)

    for method in methods:
        points = sorted([p for p in data[method] if p[0] <= 6], key=lambda x: x[0])
        threads = [p[0] for p in points]
        throughput = [p[1] for p in points]
        color = color_for(method)
        ax.plot(threads, throughput, marker=marker_for(method), linestyle="-",
                label=label_for(method), markersize=3,
                **({"color": color} if color else {}))

    ax.set_xlabel("Number of Writer Threads")
    ax.set_ylabel("Throughput (records/s)")
    ax.set_xticks(sorted({p[0] for pts in data.values() for p in pts if p[0] <= 6}))
    ax.set_ylim(bottom=0)
    ax.tick_params(axis="x", length=0)
    ax.tick_params(axis="y", length=2, width=0.3, direction="in")
    ax.yaxis.set_major_formatter(FuncFormatter(
        lambda x, _: "0" if x == 0 else rf"${x/1e5:g}\times 10^5$"))
    ax.legend(loc="lower left", bbox_to_anchor=(0, 1.02, 1, 0.2),
              ncol=3, mode="expand", frameon=False)
    ax.grid(False)
    fig.tight_layout()

    out_path = os.path.join(output_dir, "throughput_scaling.pdf")
    fig.savefig(out_path, dpi=150, bbox_inches="tight")
    print(f"Saved: {out_path}")
    plt.close(fig)


def plot_speedup(data, output_dir):
    """Line chart: speedup (throughput / single-thread throughput) vs num_threads."""
    methods = get_ordered_methods(data.keys())

    fig, ax = plt.subplots(figsize=(3.333, 3.333 * 0.618))

    all_threads = sorted({p[0] for pts in data.values() for p in pts})

    for method in methods:
        points = sorted(data[method], key=lambda x: x[0])
        threads = [p[0] for p in points]
        throughput = [p[1] for p in points]
        if not throughput or throughput[0] == 0:
            continue
        base = throughput[0]  # single-thread baseline
        speedup = [t / base for t in throughput]
        color = color_for(method)
        ax.plot(threads, speedup, "o-", label=label_for(method),
                **({"color": color} if color else {}))

    # Ideal linear speedup reference line
    ax.plot(all_threads, all_threads, "k--", alpha=0.3, label="Ideal")

    ax.set_xlabel("Number of Writer Threads")
    ax.set_ylabel("Speedup (vs 1 thread)")
    ax.set_xticks(all_threads)
    ax.tick_params(axis="x", length=0)
    ax.tick_params(axis="y", length=2, width=0.3, direction="in")
    ax.legend()
    ax.grid(False)
    fig.tight_layout()

    out_path = os.path.join(output_dir, "throughput_speedup.pdf")
    fig.savefig(out_path, dpi=150)
    print(f"Saved: {out_path}")
    plt.close(fig)


def load_progress_csv(csv_paths):
    """Load all rows from one or more summary CSVs grouped by (method, num_threads).

    Checkpoints are keyed by records_written (honk_player emits at fixed 1M
    boundaries); time_sec is averaged across runs per checkpoint.

    Returns: {num_threads: {method: [(time_sec, records), ...]}}
    """
    if isinstance(csv_paths, str):
        csv_paths = [csv_paths]

    # {num_threads: {method: {records: [time_sec, ...]}}}
    runs = {}
    for csv_path in csv_paths:
        for row in _read_rows(csv_path):
            method = row["method"].strip()
            nt = int(row["num_threads"])
            time_sec = float(row["time_elapsed_ms"]) / 1000
            records = int(row["records_written"])
            runs.setdefault(nt, {}).setdefault(method, {}).setdefault(records, []).append(time_sec)

    data = {}
    for nt, method_map in runs.items():
        for method, records_map in method_map.items():
            points = [(sum(times) / len(times), records) for records, times in records_map.items()]
            data.setdefault(nt, {})[method] = points
    return data


def plot_progress_per_thread(progress_data, output_dir):
    """Single PDF with one subplot per num_threads, showing write progress."""
    thread_counts = sorted(progress_data.keys())
    n = len(thread_counts)
    fig, axes = plt.subplots(1, n, figsize=(3.333, 3.333 * 0.618), sharey=True)
    if n == 1:
        axes = [axes]

    for ax, nt in zip(axes, thread_counts):
        methods_data = progress_data[nt]
        methods = get_ordered_methods(methods_data.keys())

        for method in methods:
            points = sorted(methods_data[method], key=lambda x: x[0])
            times = [p[0] for p in points]
            records = [p[1] / 1e6 for p in points]
            color = color_for(method)
            ax.plot(times, records, label=label_for(method),
                    **({"color": color} if color else {}))

        ax.set_xlabel("Time (s)")
        ax.set_title(f"{nt} thread{'s' if nt > 1 else ''}")
        ax.tick_params(axis="x", length=2, width=0.3, direction="in")
        ax.tick_params(axis="y", length=2, width=0.3, direction="in")
        ax.grid(False)

    axes[0].set_ylabel("Records Written (M)")
    axes[-1].legend(loc="lower right")
    fig.tight_layout()

    out_path = os.path.join(output_dir, "write_progress_combined.pdf")
    fig.savefig(out_path, dpi=150)
    print(f"Saved: {out_path}")
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(
        description="Plot multi-threaded write throughput scaling. "
                    "Pass multiple CSVs to average across runs."
    )
    parser.add_argument("csv", nargs="+", help="Path(s) to summary CSV file(s)")
    parser.add_argument(
        "-o", "--output-dir", default=None,
        help="Output directory (default: same as first CSV)"
    )
    args = parser.parse_args()

    output_dir = args.output_dir or os.path.dirname(args.csv[0])
    os.makedirs(output_dir, exist_ok=True)

    if len(args.csv) > 1:
        print(f"Averaging across {len(args.csv)} CSVs:")
        for p in args.csv:
            print(f"  - {p}")

    data = load_summary_csv(args.csv)
    if not data:
        print("No data found.")
        return

    plot_throughput_scaling(data, output_dir)


if __name__ == "__main__":
    main()
