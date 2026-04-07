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


METHOD_ORDER = [
    "no-index",
    "si-ck",
    "si-lu",
    "bitlsm_rho0.1",
]
METHOD_LABELS = {
    "no-index": "No Index",
    "si-ck": "CK",
    "si-lu": "LU",
    "bitlsm_rho0.1": r"BitLSM ($\rho$=0.1)",
    "bitlsm_rho0.05": r"BitLSM ($\rho$=0.05)",
    "bitlsm_rho0.2": r"BitLSM ($\rho$=0.2)",
}
METHOD_COLORS = {
    "bitlsm_rho0.2": "#F08C7C",
    "bitlsm_rho0.1": "#E04040",
    "bitlsm_rho0.05": "#9B1B1B",
    "si-lu": "#4CC850",
    "si-ck": "#9888B8",
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
        # e.g., write_seq_2025_all_bitlsm_rho0.1_write_log.csv
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


def load_summary_csv(csv_path: str):
    """Load a summary CSV with columns: method, num_threads, time_elapsed_ms, records_written.

    Returns: {method: [(num_threads, throughput), ...]}
    """
    data = {}
    with open(csv_path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            method = row["method"].strip()
            nt = int(row["num_threads"])
            time_ms = float(row["time_elapsed_ms"])
            records = int(row["records_written"])
            throughput = records / (time_ms / 1000) if time_ms > 0 else 0
            data.setdefault(method, []).append((nt, throughput))
    return data


def get_ordered_methods(data_keys):
    ordered = [m for m in METHOD_ORDER if m in data_keys]
    remaining = sorted(set(data_keys) - set(METHOD_ORDER))
    return ordered + remaining


def label_for(method):
    return METHOD_LABELS.get(method, method)


def color_for(method):
    return METHOD_COLORS.get(method, None)


def plot_throughput_scaling(data, output_dir):
    """Line chart: throughput (M records/sec) vs num_threads per method."""
    methods = get_ordered_methods(data.keys())

    fig, ax = plt.subplots(figsize=(7, 7 * 0.618))

    for method in methods:
        points = sorted(data[method], key=lambda x: x[0])
        threads = [p[0] for p in points]
        throughput = [p[1] / 1e6 for p in points]
        color = color_for(method)
        ax.plot(threads, throughput, "o-", label=label_for(method),
                **({"color": color} if color else {}))

    ax.set_xlabel("Number of Writer Threads")
    ax.set_ylabel("Throughput (M records/sec)")
    ax.set_title("Write Throughput Scaling")
    ax.set_xticks(sorted({p[0] for pts in data.values() for p in pts}))
    ax.legend()
    ax.grid(alpha=0.3)
    fig.tight_layout()

    out_path = os.path.join(output_dir, "throughput_scaling.png")
    fig.savefig(out_path, dpi=150)
    print(f"Saved: {out_path}")
    plt.close(fig)


def plot_speedup(data, output_dir):
    """Line chart: speedup (throughput / single-thread throughput) vs num_threads."""
    methods = get_ordered_methods(data.keys())

    fig, ax = plt.subplots(figsize=(7, 7 * 0.618))

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
    ax.set_title("Write Throughput Speedup")
    ax.set_xticks(all_threads)
    ax.legend()
    ax.grid(alpha=0.3)
    fig.tight_layout()

    out_path = os.path.join(output_dir, "throughput_speedup.png")
    fig.savefig(out_path, dpi=150)
    print(f"Saved: {out_path}")
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(
        description="Plot multi-threaded write throughput scaling"
    )
    parser.add_argument("csv", help="Path to summary CSV file")
    parser.add_argument(
        "-o", "--output-dir", default=None,
        help="Output directory (default: same as CSV)"
    )
    args = parser.parse_args()

    output_dir = args.output_dir or os.path.dirname(args.csv)
    os.makedirs(output_dir, exist_ok=True)

    data = load_summary_csv(args.csv)
    if not data:
        print("No data found.")
        return

    plot_throughput_scaling(data, output_dir)
    plot_speedup(data, output_dir)


if __name__ == "__main__":
    main()
