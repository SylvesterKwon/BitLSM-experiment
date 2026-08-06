#!/usr/bin/env python3
"""Plot DB size and write time comparison across methods for seq_write experiment."""

import argparse
import csv
import os
import re

import matplotlib.pyplot as plt
import numpy as np

plt.rcParams.update({"font.size": 6})


SCHEMAS = ["a1", "a2", "a4", "a8", "a16", "a32"]
SCHEMA_TICK_LABELS = ["a=1", "a=2", "a=4", "a=8", "a=16", "a=32"]

METHOD_ORDER = [
    "no-index",
    "si-lu",
    "si-ck",
    "si-eager",
    "embedded",
    "sai",
    "bitlsm_rho0.03",
    "bitlsm_rho0.01",
    "bitlsm_rho0.003",
]
METHOD_LABELS = {
    "no-index": "No Index",
    "si-lu": "Lazy",
    "si-ck": "Composite",
    "si-eager": "SI-Eager",
    "embedded": "Bloom + Zone Map",
    "sai": "SAI",
    "bitlsm_rho0.03": r"BitLSM ($\rho$=0.03)",
    "bitlsm_rho0.01": r"BitLSM ($\rho$=0.01)",
    "bitlsm_rho0.003": r"BitLSM ($\rho$=0.003)",
}
METHOD_COLORS = {
    "bitlsm_rho0.03": "#F08C7C",
    "bitlsm_rho0.01": "#E04040",
    "bitlsm_rho0.003": "#9B1B1B",
    "si-lu": "#4CC850",
    "si-ck": "#9888B8",
    "embedded": "#1FA8A0",
    "sai": "#3060C0",
}


MISSING = (float("nan"), 0, float("nan"))


def _schema_slot(schema_field: str):
    """Map a schema stem like 'default_a8_c1000' to its index in SCHEMAS."""
    m = re.search(r"_(a\d+)_", schema_field)
    if not m:
        return None
    try:
        return SCHEMAS.index(m.group(1))
    except ValueError:
        return None


def load_final_rows(csv_path: str):
    """Load final row (with db_size_bytes) per run, aligned to SCHEMAS.

    A CSV carrying a `schema` column pins each run to its attribute count, so a
    resumed (--start-from) or method-filtered sweep still lands on the right
    x position. Older CSVs have no such column and are read positionally, the
    way they were written. Absent runs become NaN and simply do not draw.
    Returns {method: [(time_ms, records, db_size_bytes), ...]}.
    """
    data: dict[str, list[tuple[float, int, int]]] = {}
    with open(csv_path, newline="") as f:
        # Handle possible CR line endings
        content = f.read().replace("\r\n", "\n").replace("\r", "\n")
    reader = csv.DictReader(content.strip().splitlines())
    keyed = reader.fieldnames is not None and "schema" in reader.fieldnames
    for row in reader:
        db_bytes = row.get("db_size_bytes", "").strip()
        if not db_bytes:
            continue
        method = row["method"].strip()
        entry = (float(row["time_elapsed_ms"]), int(row["records_written"]),
                 int(db_bytes))
        if keyed:
            slot = _schema_slot(row.get("schema", "").strip())
            if slot is None:
                print(f"  [warn] unknown schema {row.get('schema')!r} for "
                      f"{method}; row skipped")
                continue
            runs = data.setdefault(method, [MISSING] * len(SCHEMAS))
            runs[slot] = entry
        else:
            data.setdefault(method, []).append(entry)
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
        vals_gb = [s[2] / 1e9 for s in data[method]]
        padded = vals_gb + [float("nan")] * (len(SCHEMAS) - len(vals_gb))
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
                )

    ax.set_ylabel("DB Size (GB)")

    ax.set_xticks(x)
    ax.set_xticklabels(SCHEMA_TICK_LABELS)
    ax.tick_params(axis="x", length=0)
    ax.tick_params(axis="y", length=2, width=0.3, direction="in")
    ax.legend()
    ax.grid(False)
    fig.tight_layout()

    out_path = os.path.join(output_dir, "db_size_comparison.pdf")
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
                )

    ax.set_ylabel("Write Time (seconds)")

    ax.set_xticks(x)
    ax.set_xticklabels(SCHEMA_TICK_LABELS)
    ax.tick_params(axis="x", length=0)
    ax.tick_params(axis="y", length=2, width=0.3, direction="in")
    ax.legend()
    ax.grid(False)
    fig.tight_layout()

    out_path = os.path.join(output_dir, "write_time_comparison.pdf")
    fig.savefig(out_path, dpi=150)
    print(f"Saved: {out_path}")
    plt.close(fig)


def plot_normalized(data, output_dir):
    """Two-column figure: DB size and write time normalized to no-index."""
    methods = get_ordered_methods(data.keys())
    if "no-index" not in data:
        print("Skipping normalized plot: no-index data missing.")
        return

    size_baseline = [s[2] for s in data["no-index"]]
    size_baseline_gb = [b / 1e9 for b in size_baseline]
    time_baseline = [s[0] for s in data["no-index"]]
    time_baseline_sec = [b / 1000 for b in time_baseline]

    subplot_w = 7 / 2
    subplot_h = subplot_w * 0.618
    fig, (ax_time, ax_size) = plt.subplots(1, 2, figsize=(7, subplot_h))
    x = np.arange(len(SCHEMAS))
    n = len(methods)
    width = 0.8 / n

    for ax, get_raw, bl, annot_vals, annot_fmt, ylabel in [
        (ax_time, lambda m: [s[0] for s in data[m]], time_baseline,
         time_baseline_sec, lambda v: f"  {v:.1f}s", "Total Write Time Ratio"),
        (ax_size, lambda m: [s[2] for s in data[m]], size_baseline,
         size_baseline_gb, lambda v: f"  {v:.1f} GB", "DB Size Ratio"),
    ]:
        for i, method in enumerate(methods):
            raw = get_raw(method)
            ratios = [r / b if b else float("nan") for r, b in zip(raw, bl)]
            padded = ratios + [float("nan")] * (len(SCHEMAS) - len(ratios))
            offset = (i - n / 2 + 0.5) * width
            color = color_for(method)
            bars = ax.bar(x + offset, padded, width, label=label_for(method),
                          **({"color": color} if color else {}))
            if method == "no-index":
                for bar, av in zip(bars, annot_vals):
                    ax.text(
                        bar.get_x() + bar.get_width() / 2,
                        bar.get_height(),
                        annot_fmt(av),
                        ha="center",
                        va="bottom",
                        rotation=90,
                    )

            ax.set_ylabel(ylabel)
        ax.set_xticks(x)
        ax.set_xticklabels(SCHEMA_TICK_LABELS)
        ax.tick_params(axis="x", length=0)
        ax.tick_params(axis="y", length=2, width=0.3, direction="in")
        ax.grid(False)

    handles, labels = ax_time.get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncol=len(methods),
               frameon=False)
    fig.tight_layout(rect=[0, 0, 1, 0.90], w_pad=2.0)

    out_path = os.path.join(output_dir, "normalized_comparison.pdf")
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
    ax.set_ylabel("Throughput (M records/s)")

    ax.legend()
    ax.grid(alpha=0.3)
    fig.tight_layout()

    out_path = os.path.join(output_dir, f"write_throughput_{schema}.pdf")
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
    plot_normalized(data, output_dir)

    if args.throughput_schema is not None:
        timeseries = load_timeseries(args.csv)
        plot_write_throughput(timeseries, args.throughput_schema, output_dir)


if __name__ == "__main__":
    main()
