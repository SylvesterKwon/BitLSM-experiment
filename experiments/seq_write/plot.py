#!/usr/bin/env python3
"""Plot DB size and write time comparison across methods for seq_write experiment."""

import argparse
import csv
import os

import matplotlib.pyplot as plt
import numpy as np


SCHEMAS = ["a1", "a2", "a4", "a8", "a16", "a32"]

METHOD_ORDER = [
    "no-index",
    "si-ck",
    "si-lu",
    "si-eager",
    "bitlsm_rho0.2",
    "bitlsm_rho0.1",
    "bitlsm_rho0.05",
]
METHOD_LABELS = {
    "no-index": "No Index",
    "si-ck": "CK",
    "si-lu": "LU",
    "si-eager": "SI-Eager",
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
}


def load_final_rows(csv_path: str):
    """Load final row (with db_size_bytes) per run.

    Each method has up to len(SCHEMAS) runs in order.
    Returns {method: [(time_ms, records, db_size_bytes), ...]}.
    """
    data: dict[str, list[tuple[float, int, int]]] = {}
    with open(csv_path, newline="") as f:
        # Handle possible CR line endings
        content = f.read().replace("\r\n", "\n").replace("\r", "\n")
    reader = csv.DictReader(content.strip().splitlines())
    for row in reader:
        db_bytes = row.get("db_size_bytes", "").strip()
        if not db_bytes:
            continue
        method = row["method"].strip()
        time_ms = float(row["time_elapsed_ms"])
        records = int(row["records_written"])
        db_size = int(db_bytes)
        data.setdefault(method, []).append((time_ms, records, db_size))
    return data


def load_timeseries(csv_path: str):
    """Load full time series per (method, run_index).

    Returns {method: [[(time_ms, records), ...], ...]}.
    """
    series: dict[str, list[list[tuple[float, int]]]] = {}
    with open(csv_path, newline="") as f:
        content = f.read().replace("\r\n", "\n").replace("\r", "\n")
    reader = csv.DictReader(content.strip().splitlines())
    for row in reader:
        method = row["method"].strip()
        time_ms = float(row["time_elapsed_ms"])
        records = int(row["records_written"])
        if method not in series:
            series[method] = [[]]
        # Detect new run: records_written decreased or equal to previous with db_size
        cur_run = series[method][-1]
        if cur_run and records <= cur_run[-1][1]:
            series[method].append([])
            cur_run = series[method][-1]
        cur_run.append((time_ms, records))
    return series


def get_ordered_methods(data_keys):
    ordered = [m for m in METHOD_ORDER if m in data_keys]
    remaining = sorted(set(data_keys) - set(METHOD_ORDER))
    return ordered + remaining


def label_for(method):
    return METHOD_LABELS.get(method, method)


def color_for(method):
    return METHOD_COLORS.get(method, None)


def plot_db_size(data, output_dir):
    """Bar chart: DB size (GiB) per method per schema."""
    methods = get_ordered_methods(data.keys())

    fig, ax = plt.subplots(figsize=(7, 7 * 0.618))
    x = np.arange(len(SCHEMAS))
    n = len(methods)
    width = 0.8 / n

    for i, method in enumerate(methods):
        vals_gib = [s[2] / (1024**3) for s in data[method]]
        padded = vals_gib + [float("nan")] * (len(SCHEMAS) - len(vals_gib))
        offset = (i - n / 2 + 0.5) * width
        color = color_for(method)
        bars = ax.bar(x + offset, padded, width, label=label_for(method),
                      **({"color": color} if color else {}))
        for bar, v in zip(bars, padded):
            if not np.isnan(v):
                ax.text(
                    bar.get_x() + bar.get_width() / 2,
                    bar.get_height(),
                    f"{v:.1f}",
                    ha="center",
                    va="bottom",
                    fontsize=7,
                )

    ax.set_xlabel("Schema (number of attributes)")
    ax.set_ylabel("DB Size (GiB)")
    ax.set_title("DB Size After Sequential Writes")
    ax.set_xticks(x)
    ax.set_xticklabels(SCHEMAS)
    ax.legend()
    ax.grid(axis="y", alpha=0.3)
    fig.tight_layout()

    out_path = os.path.join(output_dir, "db_size_comparison.png")
    fig.savefig(out_path, dpi=150)
    print(f"Saved: {out_path}")
    plt.close(fig)


def plot_write_time(data, output_dir):
    """Bar chart: total write time (seconds) per method per schema."""
    methods = get_ordered_methods(data.keys())

    fig, ax = plt.subplots(figsize=(7, 7 * 0.618))
    x = np.arange(len(SCHEMAS))
    n = len(methods)
    width = 0.8 / n

    for i, method in enumerate(methods):
        vals_sec = [s[0] / 1000 for s in data[method]]
        padded = vals_sec + [float("nan")] * (len(SCHEMAS) - len(vals_sec))
        offset = (i - n / 2 + 0.5) * width
        color = color_for(method)
        bars = ax.bar(x + offset, padded, width, label=label_for(method),
                      **({"color": color} if color else {}))
        for bar, v in zip(bars, padded):
            if not np.isnan(v):
                ax.text(
                    bar.get_x() + bar.get_width() / 2,
                    bar.get_height(),
                    f"{v:.0f}",
                    ha="center",
                    va="bottom",
                    fontsize=7,
                )

    ax.set_xlabel("Schema (number of attributes)")
    ax.set_ylabel("Write Time (seconds)")
    ax.set_title("Total Write Time for Sequential Writes")
    ax.set_xticks(x)
    ax.set_xticklabels(SCHEMAS)
    ax.legend()
    ax.grid(axis="y", alpha=0.3)
    fig.tight_layout()

    out_path = os.path.join(output_dir, "write_time_comparison.png")
    fig.savefig(out_path, dpi=150)
    print(f"Saved: {out_path}")
    plt.close(fig)


def plot_write_throughput(timeseries, schema_idx, output_dir):
    """Line chart: write throughput (records/sec) over time for one schema."""
    schema = SCHEMAS[schema_idx]
    methods = get_ordered_methods(timeseries.keys())

    fig, ax = plt.subplots(figsize=(7, 7 * 0.618))

    for method in methods:
        runs = timeseries[method]
        if schema_idx >= len(runs):
            continue
        points = runs[schema_idx]
        times_sec = np.array([p[0] / 1000 for p in points])
        records = np.array([p[1] for p in points])
        # Instantaneous throughput between checkpoints
        dt = np.diff(times_sec)
        dr = np.diff(records)
        throughput = dr / dt  # records/sec
        mid_records = (records[:-1] + records[1:]) / 2
        color = color_for(method)
        ax.plot(mid_records / 1e6, throughput / 1e6, label=label_for(method),
                **({"color": color} if color else {}))

    ax.set_xlabel("Records Written (M)")
    ax.set_ylabel("Throughput (M records/sec)")
    ax.set_title(f"Write Throughput — Schema {schema}")
    ax.legend()
    ax.grid(alpha=0.3)
    fig.tight_layout()

    out_path = os.path.join(output_dir, f"write_throughput_{schema}.png")
    fig.savefig(out_path, dpi=150)
    print(f"Saved: {out_path}")
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(
        description="Plot DB size and write time comparison"
    )
    parser.add_argument("csv", help="Path to master CSV file")
    parser.add_argument(
        "-o", "--output-dir", default=None, help="Output directory (default: same as CSV)"
    )
    parser.add_argument(
        "--throughput-schema",
        type=int,
        default=None,
        metavar="IDX",
        help="Schema index (0-5) to plot throughput curve. Omit to skip.",
    )
    args = parser.parse_args()

    output_dir = args.output_dir or os.path.dirname(args.csv)
    os.makedirs(output_dir, exist_ok=True)

    data = load_final_rows(args.csv)
    if not data:
        print("No db_size_bytes data found.")
        return

    plot_db_size(data, output_dir)
    plot_write_time(data, output_dir)

    if args.throughput_schema is not None:
        timeseries = load_timeseries(args.csv)
        plot_write_throughput(timeseries, args.throughput_schema, output_dir)


if __name__ == "__main__":
    main()
