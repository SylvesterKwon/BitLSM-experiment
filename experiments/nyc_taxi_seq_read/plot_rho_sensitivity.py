#!/usr/bin/env python3
"""Plot the rho sensitivity supplementary experiment (single-column figures).

Two figures, both with rho on a log x-axis running coarse (left) to fine
(right), so moving right always means spending more on the index:

  1. rho_sensitivity_latency.pdf   — mean query latency per predicate count c.
     The optimum rho moves with c, and past it the curve turns back up because
     the SABI block keeps growing after pruning has saturated. c=1 has not
     turned by the edge of the grid, so its optimum lies below the smallest rho
     measured.
  2. rho_sensitivity_ingestion.pdf — what that resolution costs to build:
     DB size and background CPU side by side, both normalized to no-index.

Latency comes from the read sweep, ingestion cost from the write sweep, so the
two live in different result directories.

Usage:
    python3 experiments/nyc_taxi_seq_read/plot_rho_sensitivity.py <read_dir>
    python3 experiments/nyc_taxi_seq_read/plot_rho_sensitivity.py <read_dir> \
        -w <write_dir> -o <output_dir>
"""

import argparse
import csv
import glob
import os
import re
import statistics as st

import matplotlib.pyplot as plt
import matplotlib.ticker as mticker

plt.rcParams.update({"font.size": 6})

COLUMN_W = 3.333          # single text column, inches
GOLDEN = 0.618
HZ = 100                  # USER_HZ for /proc/<pid>/task/<tid>/stat ticks

# c is a property of the query, not a method, but keeping the BitLSM red family
# makes these figures read as part of the same series as the other plots.
C_COLORS = {1: "#9B1B1B", 2: "#E04040", 3: "#F08C7C"}
C_MARKERS = {1: "o", 2: "s", 3: "D"}

# passenger_count saturates at its distinct-value count (10 bins) for every rho,
# so a predicate on it costs bitmap work and buys no extra pruning no matter how
# fine the index gets. The query sets sample it at very different rates, which
# is enough on its own to invert the c=2 / c=3 ordering at fine rho.
SATURATING_ATTRS = {"passenger_count"}

READ_RE = re.compile(r"_k(\d)_.*_bitlsm_rho([\d.]+)_read_log\.csv$")
THREAD_RE = re.compile(r"_(no-index|bitlsm_rho[\d.]+)_thread_log\.csv$")


def read_cells(read_dir, refinable_only):
    """{(c, rho): [latency_s, ...]} from the read sweep's per-query logs."""
    cells = {}
    for path in sorted(glob.glob(os.path.join(read_dir, "*_read_log.csv"))):
        m = READ_RE.search(os.path.basename(path))
        if not m:
            continue
        c, rho = int(m.group(1)), float(m.group(2))
        lat = []
        with open(path) as f:
            for row in csv.DictReader(f):
                if refinable_only:
                    attrs = {a.strip() for a in row["filter_attrs"].split(",")}
                    if attrs & SATURATING_ATTRS:
                        continue
                lat.append(int(row["time_elapsed_ms"]) / 1000)
        if lat:
            cells[(c, rho)] = lat
    return cells


def ingestion_costs(write_dir):
    """({rho: size_ratio}, {rho: bg_cpu_ratio}) normalized to no-index."""
    sizes = {}
    size_csv = os.path.join(write_dir, "db_size.csv")
    if os.path.exists(size_csv):
        for row in csv.DictReader(open(size_csv)):
            key = "no-index" if row["method"] == "no-index" else float(row["rho"])
            sizes[key] = int(row["db_size_bytes"])

    bg = {}
    for path in glob.glob(os.path.join(write_dir, "*_thread_log.csv")):
        m = THREAD_RE.search(os.path.basename(path))
        if not m:
            continue
        tag = m.group(1)
        key = "no-index" if tag == "no-index" else float(tag[len("bitlsm_rho"):])
        rows = list(csv.DictReader(open(path)))
        if not rows:
            continue
        # Ticks are cumulative, so the final checkpoint holds the run total.
        last = rows[-1]["timestamp_ns"]
        bg[key] = sum(int(r["utime_ticks"]) + int(r["stime_ticks"])
                      for r in rows
                      if r["timestamp_ns"] == last
                      and r["comm"].startswith("rocksdb")) / HZ

    def normalize(d):
        base = d.get("no-index")
        if not base:
            return {}
        return {k: v / base for k, v in d.items() if k != "no-index"}

    return normalize(sizes), normalize(bg)


def style_rho_axis(ax, rhos):
    """Log rho axis, coarse on the left and fine on the right.

    Only decades are labelled; the nine sampled rho values sit on the axis as
    unlabelled minor ticks, which keeps the axis readable at column width.
    """
    ax.set_xscale("log")
    ax.set_xlim(max(rhos) * 1.35, min(rhos) / 1.35)   # inverted: fine to the right
    ax.xaxis.set_major_locator(mticker.LogLocator(base=10.0))
    ax.xaxis.set_major_formatter(mticker.FuncFormatter(lambda v, _: f"{v:g}"))
    ax.xaxis.set_minor_locator(mticker.FixedLocator(rhos))
    ax.xaxis.set_minor_formatter(mticker.NullFormatter())
    ax.set_xlabel(r"$\rho$  (coarse $\rightarrow$ fine)")
    ax.tick_params(which="both", direction="in", top=False, right=False)


def plot_latency(cells, out_dir, stat, refinable_only):
    rhos = sorted({rho for _, rho in cells})
    if not rhos:
        print("  no read cells found, skipping latency figure")
        return

    fig, ax = plt.subplots(figsize=(COLUMN_W, COLUMN_W * GOLDEN))
    agg = st.mean if stat == "mean" else st.median

    for c in sorted({c for c, _ in cells}):
        xs = [r for r in rhos if (c, r) in cells]
        ys = [agg(cells[(c, r)]) for r in xs]
        ax.plot(xs, ys, marker=C_MARKERS.get(c, "o"), markersize=2.5,
                linewidth=1.0, color=C_COLORS.get(c, "#5A5A5A"),
                label=f"c = {c}", zorder=3)

    ax.set_yscale("log")
    ax.set_ylabel(f"{stat.capitalize()} Query Latency (s)")
    style_rho_axis(ax, rhos)
    ax.yaxis.set_major_formatter(mticker.FuncFormatter(lambda v, _: f"{v:g}"))
    ax.yaxis.set_minor_formatter(mticker.NullFormatter())
    ax.legend(frameon=False, handlelength=1.6, borderaxespad=0.3)

    fig.tight_layout()
    suffix = "_refinable" if refinable_only else ""
    out = os.path.join(out_dir, f"rho_sensitivity_latency{suffix}.pdf")
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  wrote {out}")


def plot_ingestion(sizes, bg, out_dir):
    rhos = sorted(set(sizes) & set(bg))
    if not rhos:
        print("  no ingestion data found, skipping ingestion figure")
        return

    fig, axes = plt.subplots(1, 2, figsize=(COLUMN_W, COLUMN_W * 0.46),
                             sharex=True)
    # No panel titles: these figures get their (a)/(b) captions in LaTeX.
    panels = [
        (axes[0], sizes, "DB Size Ratio"),
        (axes[1], bg, "Background CPU Ratio"),
    ]
    for ax, data, ylabel in panels:
        ys = [data[r] for r in rhos]
        ax.axhline(1.0, color="#5A5A5A", linewidth=0.7, linestyle=":",
                   zorder=1)
        ax.plot(rhos, ys, marker="o", markersize=2.5, linewidth=1.0,
                color="#E04040", zorder=3)
        ax.set_ylabel(ylabel)
        style_rho_axis(ax, rhos)
        lo, hi = min(ys + [1.0]), max(ys + [1.0])
        pad = (hi - lo) * 0.15 or 0.05
        ax.set_ylim(lo - pad, hi + pad)
    fig.tight_layout(pad=0.4, w_pad=0.8)
    out = os.path.join(out_dir, "rho_sensitivity_ingestion.pdf")
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  wrote {out}")


def main():
    parser = argparse.ArgumentParser(
        description="Plot the rho sensitivity experiment")
    parser.add_argument("read_dir",
                        help="Read sweep result directory (*_read_log.csv)")
    parser.add_argument("-w", "--write-dir", default=None,
                        help="Write sweep result directory (db_size.csv and "
                             "*_thread_log.csv); omit to skip figure 2")
    parser.add_argument("-o", "--output-dir", default=None,
                        help="Where to write the PDFs (default: read_dir)")
    parser.add_argument("--stat", choices=["mean", "median"], default="mean",
                        help="Per-cell statistic for the latency curves")
    parser.add_argument("--refinable-only", action="store_true",
                        help="Drop queries touching a saturating attribute "
                             "(passenger_count), which the three query sets "
                             "sample at very different rates")
    args = parser.parse_args()

    out_dir = args.output_dir or args.read_dir
    os.makedirs(out_dir, exist_ok=True)

    cells = read_cells(args.read_dir, args.refinable_only)
    counts = sorted({c for c, _ in cells})
    print(f"read cells: {len(cells)} "
          f"(c = {counts}, "
          f"{len({r for _, r in cells})} rho values, "
          f"{min((len(v) for v in cells.values()), default=0)}-"
          f"{max((len(v) for v in cells.values()), default=0)} queries each)")
    plot_latency(cells, out_dir, args.stat, args.refinable_only)

    if args.write_dir:
        sizes, bg = ingestion_costs(args.write_dir)
        print(f"ingestion: {len(sizes)} sizes, {len(bg)} CPU totals")
        plot_ingestion(sizes, bg, out_dir)
    else:
        print("no --write-dir given, skipping the ingestion figure")


if __name__ == "__main__":
    main()
