#!/usr/bin/env python3
"""Plot write throughput vs max_background_jobs.

Reads the summary CSV produced by run.py and plots throughput scaling
as max_background_jobs varies (num_threads is fixed).

Usage:
    python3 experiments/seq_write_multi_thread/plot_sweep_bg.py <summary.csv>
"""

import argparse
import csv
import os

import matplotlib.pyplot as plt

plt.rcParams.update({"font.size": 6})


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


def get_ordered_methods(data_keys):
    ordered = [m for m in METHOD_ORDER if m in data_keys]
    remaining = sorted(set(data_keys) - set(METHOD_ORDER))
    return ordered + remaining


def label_for(method):
    return METHOD_LABELS.get(method, method)


def color_for(method):
    return METHOD_COLORS.get(method, None)


def load_summary_csv(csv_path: str):
    """Load final rows (with db_size_bytes) from summary CSV.

    Returns: {method: [(max_background_jobs, throughput), ...]}
    """
    data = {}
    with open(csv_path, newline="") as f:
        content = f.read().replace("\r\n", "\n").replace("\r", "\n")
    reader = csv.DictReader(content.strip().splitlines())
    for row in reader:
        db_bytes = row.get("db_size_bytes", "").strip()
        if not db_bytes:
            continue
        method = row["method"].strip()
        bg_jobs = int(row["max_background_jobs"])
        time_ms = float(row["time_elapsed_ms"])
        records = int(row["records_written"])
        throughput = records / (time_ms / 1000) if time_ms > 0 else 0
        data.setdefault(method, []).append((bg_jobs, throughput))
    return data


def load_progress_csv(csv_path: str):
    """Load all rows from summary CSV grouped by (method, max_background_jobs).

    Returns: {max_background_jobs: {method: [(time_sec, records), ...]}}
    """
    data = {}
    with open(csv_path, newline="") as f:
        content = f.read().replace("\r\n", "\n").replace("\r", "\n")
    reader = csv.DictReader(content.strip().splitlines())
    for row in reader:
        method = row["method"].strip()
        bg_jobs = int(row["max_background_jobs"])
        time_sec = float(row["time_elapsed_ms"]) / 1000
        records = int(row["records_written"])
        data.setdefault(bg_jobs, {}).setdefault(method, []).append((time_sec, records))
    return data


def plot_throughput_scaling(data, output_dir):
    """Line chart: throughput (M records/sec) vs max_background_jobs per method."""
    methods = get_ordered_methods(data.keys())

    fig, ax = plt.subplots(figsize=(7, 7 * 0.618))

    for method in methods:
        points = sorted(data[method], key=lambda x: x[0])
        bg_jobs = [p[0] for p in points]
        throughput = [p[1] / 1e6 for p in points]
        color = color_for(method)
        ax.plot(bg_jobs, throughput, "o-", label=label_for(method),
                **({"color": color} if color else {}))

    ax.set_xlabel("Max Background Jobs")
    ax.set_ylabel("Throughput (M records/sec)")
    ax.set_xticks(sorted({p[0] for pts in data.values() for p in pts}))
    ax.tick_params(axis="x", length=0)
    ax.tick_params(axis="y", length=2, width=0.3, direction="in")
    ax.legend()
    ax.grid(False)
    fig.tight_layout()

    out_path = os.path.join(output_dir, "throughput_vs_bg_jobs.pdf")
    fig.savefig(out_path, dpi=150)
    print(f"Saved: {out_path}")
    plt.close(fig)


def plot_progress_per_bg_jobs(progress_data, output_dir):
    """Single PDF with one subplot per max_background_jobs, showing write progress."""
    bg_counts = sorted(progress_data.keys())
    n = len(bg_counts)
    fig, axes = plt.subplots(1, n, figsize=(4 * n, 4), sharey=True)
    if n == 1:
        axes = [axes]

    for ax, bg in zip(axes, bg_counts):
        methods_data = progress_data[bg]
        methods = get_ordered_methods(methods_data.keys())

        for method in methods:
            points = sorted(methods_data[method], key=lambda x: x[0])
            times = [p[0] for p in points]
            records = [p[1] / 1e6 for p in points]
            color = color_for(method)
            ax.plot(times, records, label=label_for(method),
                    **({"color": color} if color else {}))

        ax.set_xlabel("Time (sec)")
        ax.tick_params(axis="x", length=2, width=0.3, direction="in")
        ax.tick_params(axis="y", length=2, width=0.3, direction="in")
        ax.grid(False)

    axes[0].set_ylabel("Records Written (M)")
    axes[-1].legend(loc="lower right")
    fig.tight_layout()

    out_path = os.path.join(output_dir, "write_progress_vs_bg_jobs.pdf")
    fig.savefig(out_path, dpi=150)
    print(f"Saved: {out_path}")
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(
        description="Plot write throughput vs max_background_jobs"
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

    progress_data = load_progress_csv(args.csv)
    plot_progress_per_bg_jobs(progress_data, output_dir)


if __name__ == "__main__":
    main()
