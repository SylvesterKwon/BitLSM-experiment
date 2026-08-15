#!/usr/bin/env python3
"""Plot DB size and write time comparison across methods for nyc_taxi_seq_write.

Mirrors experiments/seq_write/plot.py, minus its schema axis: the taxi trace has
one fixed schema, so methods sit on the x axis directly instead of being grouped
per attribute count.

Two inputs, because the taxi write logs carry less than the synthetic sweep's
master CSV:
  - result_dir: one `*_write_log.csv` per run (time_elapsed_ms, records_written)
  - --db-path:  the loaded DBs, measured on disk — the write logs have no
                db_size_bytes column, so size has to come from the filesystem

Usage:
    python3 experiments/nyc_taxi_seq_write/plot.py <result_dir>
    python3 experiments/nyc_taxi_seq_write/plot.py <result_dir> --db-path /scratch/honk/uuid
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
    "si-lu_strategy_im",
    "si-ck_strategy_im",
    "embedded_bloom_bits10",
    "sai_il2",
    "bitlsm_rho0.03",
    "bitlsm_rho0.01",
    "bitlsm_rho0.003",
]
METHOD_LABELS = {
    "no-index": "No Index",
    "si-lu_strategy_im": "Lazy",
    "si-ck_strategy_im": "Composite",
    "embedded_bloom_bits10": "Bloom + Zone Map",
    "sai_il2": "SAI",
    "bitlsm_rho0.03": r"BitLSM ($\rho$=0.03)",
    "bitlsm_rho0.01": r"BitLSM ($\rho$=0.01)",
    "bitlsm_rho0.003": r"BitLSM ($\rho$=0.003)",
}
METHOD_COLORS = {
    "no-index": "#808080",
    "si-lu_strategy_im": "#4CC850",
    "si-ck_strategy_im": "#9888B8",
    "embedded_bloom_bits10": "#1FA8A0",
    "sai_il2": "#3060C0",
    "bitlsm_rho0.03": "#F08C7C",
    "bitlsm_rho0.01": "#E04040",
    "bitlsm_rho0.003": "#9B1B1B",
}

# Pattern: {workload}_{method}_write_log.csv. The workload stem carries
# underscores of its own, so the method is matched from the right by shape
# rather than split off positionally.
FILE_PATTERN = re.compile(
    r"_(no-index"
    r"|bitlsm_rho[\d.]+"
    r"|si-ck_strategy_\w+"
    r"|si-lu_strategy_\w+"
    r"|si-eager\w*"
    r"|embedded_bloom_bits\d+"
    r"|sai_il\d+)_write_log\.csv$"
)

# DB path params, mirroring DB_PARAMS in the runner: only rho / bloom_bits
# namespace a DB directory. read_strategy and intersection_limit do not, so
# those methods share the "default" directory. Matched as whole tokens —
# bloom_bits10 contains an underscore, so splitting the method key on "_"
# would tear it in half.
DB_PARAM_PATTERN = re.compile(r"(rho[\d.]+|bloom_bits\d+)")


def load_result_dir(result_dir):
    """Load the final checkpoint of every write log in result_dir.

    The last row is the end-to-end figure (it includes the wait-for-compact
    drain and close), which is what a reader means by "how long did the load
    take". Returns {method: (time_ms, records_written)}.
    """
    data = {}
    for fname in sorted(os.listdir(result_dir)):
        m = FILE_PATTERN.search(fname)
        if not m:
            continue
        rows = []
        with open(os.path.join(result_dir, fname), newline="") as f:
            for row in csv.DictReader(f):
                rows.append(row)
        if not rows:
            print(f"  [warn] {fname} has no data rows; skipped")
            continue
        last = rows[-1]
        data[m.group(1)] = (float(last["time_elapsed_ms"]),
                            int(last["records_written"]))
    return data


def db_dir_for(method):
    """Map a method key to its `{binding}/{params}` directory under --db-path."""
    binding = method.split("_")[0]
    params = DB_PARAM_PATTERN.findall(method)
    return os.path.join(binding, "_".join(params) if params else "default")


def dir_size_bytes(path):
    """Total size of the files under path (apparent size, like `du -sb`)."""
    total = 0
    for root, _dirs, files in os.walk(path):
        for fn in files:
            try:
                total += os.path.getsize(os.path.join(root, fn))
            except OSError:
                pass
    return total


def load_db_sizes(methods, db_path):
    """Measure each method's DB on disk. Missing DBs are simply left out."""
    sizes = {}
    for method in methods:
        path = os.path.join(db_path, db_dir_for(method))
        if not os.path.isdir(path):
            print(f"  [warn] no DB at {path} for {method}; size bar omitted")
            continue
        sizes[method] = dir_size_bytes(path)
    return sizes


def get_ordered_methods(data_keys):
    ordered = [m for m in METHOD_ORDER if m in data_keys]
    remaining = sorted(set(data_keys) - set(METHOD_ORDER))
    return ordered + remaining


def label_for(method):
    return METHOD_LABELS.get(method, method)


def color_for(method):
    return METHOD_COLORS.get(method, None)


def _bar_axis(ax, methods, values, ylabel, annot_fmt):
    """One bar per method, annotated, styled like the seq_write charts."""
    x = np.arange(len(methods))
    colors = [color_for(m) for m in methods]
    bars = ax.bar(x, values, 0.7,
                  color=[c if c else "#CCCCCC" for c in colors])
    for bar, v in zip(bars, values):
        if not np.isnan(v):
            ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height(),
                    annot_fmt(v), ha="center", va="bottom")

    ax.set_ylabel(ylabel)
    ax.set_xticks(x)
    ax.set_xticklabels([label_for(m) for m in methods],
                       rotation=30, ha="right")
    ax.tick_params(axis="x", length=0)
    ax.tick_params(axis="y", length=2, width=0.3, direction="in")
    ax.grid(False)


def plot_write_time(data, output_dir):
    """Bar chart: total write time (seconds) per method."""
    methods = get_ordered_methods(data.keys())
    vals_sec = [data[m][0] / 1000 for m in methods]

    fig, ax = plt.subplots(figsize=(7, 7 * 0.618))
    _bar_axis(ax, methods, vals_sec, "Write Time (seconds)",
              lambda v: f"{v:.0f}")
    fig.tight_layout()

    out_path = os.path.join(output_dir, "write_time_comparison.pdf")
    fig.savefig(out_path, dpi=150)
    print(f"Saved: {out_path}")
    plt.close(fig)


def plot_db_size(sizes, output_dir):
    """Bar chart: DB size (GB) per method."""
    methods = get_ordered_methods(sizes.keys())
    vals_gb = [sizes[m] / 1e9 for m in methods]

    fig, ax = plt.subplots(figsize=(7, 7 * 0.618))
    _bar_axis(ax, methods, vals_gb, "DB Size (GB)", lambda v: f"{v:.1f}")
    fig.tight_layout()

    out_path = os.path.join(output_dir, "db_size_comparison.pdf")
    fig.savefig(out_path, dpi=150)
    print(f"Saved: {out_path}")
    plt.close(fig)


def _annotate_baseline(ax, bar_height, text):
    """Write the baseline's absolute value inside its bar.

    The no-index bar already carries its ratio (1.00) above it from
    _bar_axis; stacking the absolute value at the same anchor collides with
    it. Placing it inside the bar keeps both readable and cannot overflow
    the axes.
    """
    ax.text(0, bar_height / 2, text, ha="center", va="center",
            rotation=90, color="white")


def plot_normalized(data, sizes, output_dir):
    """Two-column figure: write time and DB size normalized to no-index."""
    if "no-index" not in data:
        print("Skipping normalized plot: no-index data missing.")
        return

    time_methods = get_ordered_methods(data.keys())
    time_baseline = data["no-index"][0]
    time_ratios = [data[m][0] / time_baseline for m in time_methods]

    size_methods = get_ordered_methods(sizes.keys()) if sizes else []
    size_ratios = []
    if size_methods and "no-index" in sizes:
        size_baseline = sizes["no-index"]
        size_ratios = [sizes[m] / size_baseline for m in size_methods]
    elif sizes:
        print("Skipping size panel: no-index DB not measured.")
        size_methods = []

    ncols = 2 if size_methods else 1
    subplot_w = 7 / 2
    subplot_h = subplot_w * 0.618
    fig, axes = plt.subplots(1, ncols, figsize=(7, subplot_h), squeeze=False)

    _bar_axis(axes[0][0], time_methods, time_ratios,
              "Total Write Time Ratio", lambda v: f"{v:.2f}")
    _annotate_baseline(axes[0][0], time_ratios[0],
                       f"{time_baseline / 1000:.1f}s")
    if size_methods:
        _bar_axis(axes[0][1], size_methods, size_ratios,
                  "DB Size Ratio", lambda v: f"{v:.2f}")
        _annotate_baseline(axes[0][1], size_ratios[0],
                           f"{sizes['no-index'] / 1e9:.1f} GB")

    fig.tight_layout(w_pad=2.0)

    out_path = os.path.join(output_dir, "normalized_comparison.pdf")
    fig.savefig(out_path, dpi=150)
    print(f"Saved: {out_path}")
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(
        description="Plot DB size and write time comparison for nyc_taxi_seq_write"
    )
    parser.add_argument("result_dir", help="Path to result directory")
    parser.add_argument("-o", "--output-dir", default=None,
                        help="Output directory (default: same as result_dir)")
    parser.add_argument("--db-path", default=None, metavar="DIR",
                        help="Directory holding the loaded DBs as "
                             "{binding}/{params}, e.g. /scratch/honk/uuid. "
                             "Omit to skip the DB size charts.")
    args = parser.parse_args()

    output_dir = args.output_dir or args.result_dir
    os.makedirs(output_dir, exist_ok=True)

    data = load_result_dir(args.result_dir)
    if not data:
        print("No write log CSVs found.")
        return

    plot_write_time(data, output_dir)

    sizes = {}
    if args.db_path:
        sizes = load_db_sizes(get_ordered_methods(data.keys()), args.db_path)
        if sizes:
            plot_db_size(sizes, output_dir)

    plot_normalized(data, sizes, output_dir)


if __name__ == "__main__":
    main()
