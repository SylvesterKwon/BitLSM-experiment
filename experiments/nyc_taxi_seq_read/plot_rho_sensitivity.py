#!/usr/bin/env python3
"""Plot the rho sensitivity supplementary experiment (single-column figures).

Two figures, both with rho on a log x-axis running coarse (left) to fine
(right), so moving right always means spending more on the index:

  1. rho_sensitivity_latency.pdf   — mean query latency per predicate count c.
     The optimum rho moves with c, and past it the curve turns back up because
     the SABI block keeps growing after pruning has saturated. c=1 has not
     turned by the edge of the grid, so its optimum lies below the smallest rho
     measured.

     The curves are plotted exactly as measured. Higher c necessarily draws in
     lower-cardinality attributes, because reaching a fixed total selectivity
     with more predicates requires weaker ones -- that is a property of
     multi-predicate queries, not a sampling artifact, so it is not filtered
     out.
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
import math
import os
import re
import statistics as st

import matplotlib.pyplot as plt
import matplotlib.ticker as mticker
import matplotlib.transforms as mtransforms

plt.rcParams.update({"font.size": 6})

COLUMN_W = 3.333          # single text column, inches
GOLDEN = 0.618
HZ = 100                  # USER_HZ for /proc/<pid>/task/<tid>/stat ticks

# honk_player logs records_total as 0 for these workloads, so per-query
# selectivity has to be recovered against the known row count of the ingest.
TOTAL_ROWS = 89_892_322

# Cost-model constant for the predicted optimum rho* = (DELTA / c) * sigma^(1/c).
DELTA = 0.2

# c is a property of the query, not a method, but keeping the BitLSM red family
# makes these figures read as part of the same series as the other plots.
C_COLORS = {1: "#F09080", 2: "#E05545", 3: "#B02525", 4: "#6B0F0F"}
C_MARKERS = {1: "o", 2: "s", 3: "D", 4: "^"}

READ_RE = re.compile(r"_k(\d)_.*_bitlsm_rho([\d.]+)_read_log\.csv$")
THREAD_RE = re.compile(r"_(no-index|bitlsm_rho[\d.]+)_thread_log\.csv$")


def read_cells(read_dir):
    """({(c, rho): [latency_s, ...]}, {(c, rho): mean_rows_matched})."""
    cells, matched = {}, {}
    for path in sorted(glob.glob(os.path.join(read_dir, "*_read_log.csv"))):
        m = READ_RE.search(os.path.basename(path))
        if not m:
            continue
        c, rho = int(m.group(1)), float(m.group(2))
        lat, hits = [], []
        with open(path) as f:
            for row in csv.DictReader(f):
                lat.append(int(row["time_elapsed_ms"]) / 1000)
                hits.append(int(row["records_matched"]))
        if lat:
            cells[(c, rho)] = lat
            matched[(c, rho)] = st.mean(hits)
    return cells, matched


def measured_sigma(matched):
    """Total query selectivity, averaged over the predicate counts.

    The same query set runs at every rho, so per-c selectivity is constant down
    the rho axis; averaging the per-c values gives the single sigma the
    workload was generated to hit.
    """
    per_c = {}
    for (c, _), hits in matched.items():
        per_c.setdefault(c, []).append(hits)
    if not per_c:
        return None
    return st.mean([st.mean(v) / TOTAL_ROWS for v in per_c.values()])


def _sci(v):
    """1.04e-05 -> '10^{-5}', 5.2e-05 -> '5{\\times}10^{-5}'."""
    exp = math.floor(math.log10(v))
    mant = round(v / 10 ** exp)
    if mant == 10:                      # rounding pushed it up a decade
        mant, exp = 1, exp + 1
    return rf"10^{{{exp}}}" if mant == 1 else rf"{mant}{{\times}}10^{{{exp}}}"


def _glyph_offset(ax, label, fontsize):
    """Points to nudge `label` right so its rho sits on the line, not its box.

    Centring mathtext centres the whole box, and the superscript/subscript
    cluster hangs off the right of the rho — enough that the label reads as
    offset from the line it belongs to.
    """
    try:
        r = ax.figure.canvas.get_renderer()
        probe = ax.text(0, 0, label, fontsize=fontsize)
        full = probe.get_window_extent(renderer=r).width
        probe.set_text(r"$\rho$")
        bare = probe.get_window_extent(renderer=r).width
        probe.remove()
        return (full - bare) / 2 / ax.figure.dpi * 72     # px -> points
    except Exception:
        return 0.0


def _label_width(ax, label, fontsize):
    """Rendered width of `label` in display pixels, 0 if unmeasurable."""
    try:
        r = ax.figure.canvas.get_renderer()
        probe = ax.text(0, 0, label, fontsize=fontsize)
        w = probe.get_window_extent(renderer=r).width
        probe.remove()
        return w
    except Exception:
        return 0.0


def predicted_optimum(sigma, c):
    """rho* = (delta / c) * sigma^(1/c) — the model's best rho for c predicates."""
    return (DELTA / c) * sigma ** (1.0 / c)


def draw_optimum_refs(ax, sigma, rhos, counts):
    """Vertical rho* references, one per predicate count.

    A reference outside the measured grid gets an edge annotation instead of a
    line; the axis is inverted, so finer lies to the right.

    Labels sit on one row and only drop to a second when they would actually
    overlap, which depends on where the rho* values land — staggering
    unconditionally leaves one label hanging below its neighbours for no reason.
    Requires the x-axis to be scaled and limited already, since the row decision
    is made in display coordinates.

    Alignment is by baseline, not by box top: the superscript in "10^{-5}" is
    taller than the one in "rho^*", so box alignment would drop that annotation
    below its neighbours. Labels are also nudged right by half the
    superscript/subscript overhang so the rho glyph, not the box, is centred on
    the line.
    """
    row_y = (0.925, 0.815)
    lo, hi = min(rhos), max(rhos)
    box = dict(facecolor="white", edgecolor="none", pad=0.6)
    inside = sorted(((predicted_optimum(sigma, c), c) for c in counts
                     if lo <= predicted_optimum(sigma, c) <= hi), reverse=True)

    width = _label_width(ax, r"$\rho^*_{0}$", 5.5)
    prev_right = None
    for x, c in inside:
        color = C_COLORS.get(c, "#5A5A5A")
        label = rf"$\rho^*_{{{c}}}$"
        ax.axvline(x, color=color, linestyle=(0, (3, 2)), linewidth=0.7,
                   zorder=2)
        dx = ax.transData.transform((x, 1))[0]
        row = 1 if prev_right is not None and dx - width / 2 < prev_right else 0
        if row == 0:
            prev_right = dx + width / 2
        shift = mtransforms.offset_copy(
            ax.get_xaxis_transform(), fig=ax.figure,
            x=_glyph_offset(ax, label, 5.5), units="points")
        ax.text(x, row_y[row], label, color=color, transform=shift,
                ha="center", va="baseline", fontsize=5.5, bbox=box, zorder=5)

    for c in sorted(counts):
        x = predicted_optimum(sigma, c)
        if lo <= x <= hi:
            continue
        finer = x < lo
        ax.text(0.995 if finer else 0.005, row_y[0],
                rf"$\rho^*_{{{c}}} = {_sci(x)}$ "
                + ("$\\rightarrow$" if finer else "$\\leftarrow$"),
                color=C_COLORS.get(c, "#5A5A5A"), transform=ax.transAxes,
                ha="right" if finer else "left", va="baseline", fontsize=5.5,
                bbox=box, zorder=5)


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


def style_rho_axis(ax, rhos, label_all=False):
    """Log rho axis, coarse on the left and fine on the right.

    label_all labels every sampled rho. That fits on a full-width axis but not
    on the paired ingestion panels, which fall back to labelling decades and
    carrying the sampled values as unlabelled minor ticks.
    """
    ax.set_xscale("log")
    ax.set_xlim(max(rhos) * 1.35, min(rhos) / 1.35)   # inverted: fine to the right
    fmt = mticker.FuncFormatter(lambda v, _: f"{v:g}")
    if label_all:
        ax.xaxis.set_major_locator(mticker.FixedLocator(rhos))
        ax.xaxis.set_minor_locator(mticker.NullLocator())
    else:
        ax.xaxis.set_major_locator(mticker.LogLocator(base=10.0))
        ax.xaxis.set_minor_locator(mticker.FixedLocator(rhos))
        ax.xaxis.set_minor_formatter(mticker.NullFormatter())
    ax.xaxis.set_major_formatter(fmt)
    ax.set_xlabel(r"$\rho$  (coarse $\rightarrow$ fine)")
    ax.tick_params(which="both", direction="in", top=False, right=False)


def plot_latency(cells, matched, out_dir, stat, yscale="log"):
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

    ax.set_yscale(yscale)
    ax.set_ylabel(f"{stat.capitalize()} Query Latency (s)")
    style_rho_axis(ax, rhos, label_all=True)
    if yscale == "log":
        ax.yaxis.set_major_formatter(
            mticker.FuncFormatter(lambda v, _: f"{v:g}"))
        ax.yaxis.set_minor_formatter(mticker.NullFormatter())
    else:
        ax.set_ylim(bottom=0)

    sigma = measured_sigma(matched)
    # 임시 비활성처리.
    # if sigma:
    #     draw_optimum_refs(ax, sigma, rhos, {c for c, _ in cells})
    ax.legend(loc="lower left", bbox_to_anchor=(0, 1.02, 1, 0.2),
              ncol=len({c for c, _ in cells}), mode="expand", frameon=False,
              handlelength=1.6)

    fig.tight_layout()
    suffix = "" if yscale == "log" else f"_{yscale}"
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
    parser.add_argument("--yscale", choices=["log", "linear", "both"],
                        default="both",
                        help="Latency axis scale; 'both' writes the log figure "
                             "and a _linear companion")
    args = parser.parse_args()

    out_dir = args.output_dir or args.read_dir
    os.makedirs(out_dir, exist_ok=True)

    cells, matched = read_cells(args.read_dir)
    counts = sorted({c for c, _ in cells})
    print(f"read cells: {len(cells)} "
          f"(c = {counts}, "
          f"{len({r for _, r in cells})} rho values, "
          f"{min((len(v) for v in cells.values()), default=0)}-"
          f"{max((len(v) for v in cells.values()), default=0)} queries each)")
    scales = ["log", "linear"] if args.yscale == "both" else [args.yscale]
    for scale in scales:
        plot_latency(cells, matched, out_dir, args.stat, scale)

    if args.write_dir:
        sizes, bg = ingestion_costs(args.write_dir)
        print(f"ingestion: {len(sizes)} sizes, {len(bg)} CPU totals")
        plot_ingestion(sizes, bg, out_dir)
    else:
        print("no --write-dir given, skipping the ingestion figure")


if __name__ == "__main__":
    main()
