#!/usr/bin/env python3
"""Plot the rho sensitivity supplementary experiment (one two-column figure).

Reads the CSVs that summarize_rho_sensitivity.py writes from the read,
candidate and ingest runs, and draws rho_sensitivity.pdf: one row of three
panels, each with rho on a log x-axis running coarse (left) to fine (right),
so moving right always means spending more on the index. A dotted vertical
line marks the default, rho = 0.001.

  1. MEDIAN query latency per predicate count c. The optimum rho moves with c,
     and past it the curve turns back up because the SABI block keeps growing
     after pruning has saturated. c=1 has not turned by the edge of the grid,
     so its optimum lies below the smallest rho measured.

     The curves are plotted exactly as measured. Higher c necessarily draws in
     lower-cardinality attributes, because reaching a fixed total selectivity
     with more predicates requires weaker ones -- that is a property of
     multi-predicate queries, not a sampling artifact, so it is not filtered
     out.
  2. The mechanism: the MEDIAN candidate-to-match ratio, per c. 1 means every
     candidate the bitmaps select is a match.
  3. What each rho costs to build: ingest time (drain included) and DB size,
     each relative to No Index. Background CPU time (1.4-1.6x) would flatten
     these two on a shared axis, so the text reports it instead. Peak RSS is
     left out: it is dominated by the resident index, which the reader
     currently holds twice, so it would not read as the cost of rho.

Usage:
    python3 experiments/nyc_taxi_seq_read/summarize_rho_sensitivity.py \\
        <read_dir> <candidates_dir> <ingest_dir>
    python3 experiments/nyc_taxi_seq_read/plot_rho_sensitivity.py <read_dir> \\
        [-o <output_dir>]
"""

import argparse
import csv
import math
import os
from collections import defaultdict

import matplotlib.pyplot as plt
import matplotlib.ticker as mticker
import matplotlib.transforms as mtransforms

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

COLUMN_W = 3.333          # single text column, inches
BOX_ASPECT = 0.702        # the paper's panel shape (see CLAUDE.md)

QUERY_SUMMARY_CSV = "rho_sensitivity_query_summary.csv"
INGEST_CSV = "rho_sensitivity_ingest.csv"

# honk_player logs records_total as 0 for these workloads, so per-query
# selectivity has to be recovered against the known row count of the ingest.
TOTAL_ROWS = 89_892_322

# Cost-model constant for the predicted optimum rho* = (DELTA / c) * sigma^(1/c).
DELTA = 0.2

# c is a property of the query, not a method, but keeping the BitLSM red family
# makes these figures read as part of the same series as the other plots.
C_COLORS = {1: "#F09080", 2: "#E05545", 3: "#B02525", 4: "#6B0F0F"}
C_MARKERS = {1: "o", 2: "s", 3: "D", 4: "^"}

DEFAULT_RHO = 0.001

# The ingest costs are BitLSM's own, not methods. Every other hue already
# names a method somewhere in the paper (red BitLSM, and the c lines beside
# them; lavender, green, magenta, teal, blue, orange for the baselines), so
# the two costs share brown, a hue no method uses, in a dark and a light shade.
INGEST_STYLES = {
    "ingest_ms_vs_no_index":
        ("Ingest time", dict(color="#7A4A2A", marker="o")),
    "db_size_vs_no_index":
        ("DB size", dict(color="#C49A6C", marker="^")),
}


def read_csv_rows(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def load_query_cells(summary_dir):
    """{(c, rho): summary row with numeric fields as floats}."""
    cells = {}
    for r in read_csv_rows(os.path.join(summary_dir, QUERY_SUMMARY_CSV)):
        row = {k: float(v) if v not in ("",) and k != "c" else v
               for k, v in r.items()}
        cells[(int(r["c"]), float(r["rho"]))] = row
    return cells


def load_ingest(summary_dir):
    """({rho: row}, no_index_row) with numeric fields as floats."""
    path = os.path.join(summary_dir, INGEST_CSV)
    if not os.path.exists(path):
        return {}, None
    arms, base = {}, None
    for r in read_csv_rows(path):
        row = {k: (float(v) if v != "" and k != "method" else v)
               for k, v in r.items()}
        if r["method"] == "no-index":
            base = row
        else:
            arms[float(r["rho"])] = row
    return arms, base


def measured_sigma(cells):
    """Total query selectivity, averaged over the predicate counts.

    The same query set runs at every rho, so per-c selectivity is constant down
    the rho axis; averaging the per-c values gives the single sigma the
    workload was generated to hit.
    """
    per_c = defaultdict(list)
    for (c, _), row in cells.items():
        per_c[c].append(row["mean_records_matched"])
    if not per_c:
        return None
    return sum(sum(v) / len(v) / TOTAL_ROWS for v in per_c.values()) / len(per_c)


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


def style_rho_axis(ax, rhos, label_all=False, decades=False, xlabel=True):
    """Log rho axis, coarse on the left and fine on the right.

    Every sampled rho gets a tick. The labels would collide even across a full
    column, so only every other one is labelled, coarsest first and ending on
    the finest: the measurement points stay visible and the labels stop
    colliding. A half-column panel is too narrow even for that and labels the
    decades only (decades=True); label_all labels them all, for a wider figure.
    """
    ax.set_xscale("log")
    ax.set_xlim(max(rhos) * 1.35, min(rhos) / 1.35)   # inverted: fine to the right
    ax.xaxis.set_major_locator(mticker.FixedLocator(rhos))
    ax.xaxis.set_minor_locator(mticker.NullLocator())
    shown = sorted(rhos, reverse=True)
    if decades:
        shown = [r for r in shown
                 if abs(math.log10(r) - round(math.log10(r))) < 1e-9]
    elif not label_all:
        shown = shown[::2]

    def fmt(v, _):
        hit = [r for r in shown if abs(math.log(v / r)) < 1e-6]
        return f"{hit[0]:g}" if hit else ""
    ax.xaxis.set_major_formatter(mticker.FuncFormatter(fmt))
    if xlabel:
        ax.set_xlabel(r"$\rho$  (coarse $\rightarrow$ fine)")
    ax.tick_params(which="both", top=False, right=False)


def log_y(ax):
    """Log y labelled at the decades as powers of ten (10^4, not 10000)."""
    ax.set_yscale("log")
    ax.yaxis.set_major_locator(mticker.LogLocator(base=10.0))
    ax.yaxis.set_major_formatter(mticker.LogFormatterMathtext())
    ax.yaxis.set_minor_formatter(mticker.NullFormatter())


def save(fig, out_dir, name):
    out = os.path.join(out_dir, name)
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  wrote {out}")


def c_legend(fig_or_ax, handles, labels, **kw):
    fig_or_ax.legend(handles, labels, ncol=len(labels), frameon=False,
                     handlelength=1.6, **kw)


def plot_row(cells, arms, out_dir, stat):
    rhos = sorted({rho for _, rho in cells})
    counts = sorted({c for c, _ in cells})
    if not rhos:
        print("  no read cells found, skipping the figure")
        return

    fig, axes = plt.subplots(1, 3, figsize=(7, 1.85))
    fig.subplots_adjust(left=0.07, right=0.995, bottom=0.16, top=0.86,
                        wspace=0.42)
    for ax in axes:
        ax.set_box_aspect(BOX_ASPECT)
        ax.axvline(DEFAULT_RHO, color="#5A5A5A", linewidth=0.5,
                   linestyle=":", zorder=1)

    query_panels = [
        (axes[0], f"{stat}_latency_ms",
         f"{stat.capitalize()} query latency (ms)"),
        (axes[1], "median_candidate_ratio", "Median candidate-to-match"),
    ]
    for ax, col, ylabel in query_panels:
        for c in counts:
            xs = [r for r in rhos if (c, r) in cells]
            ax.plot(xs, [cells[(c, r)][col] for r in xs],
                    marker=C_MARKERS.get(c, "o"), markersize=2.5,
                    linewidth=1.0, color=C_COLORS.get(c, "#5A5A5A"),
                    label=f"c = {c}", zorder=3)
        ax.set_ylabel(ylabel)
        log_y(ax)
    axes[1].set_ylim(bottom=0.6)
    sigma = measured_sigma(cells)
    # 임시 비활성처리.
    # if sigma:
    #     draw_optimum_refs(axes[0], sigma, rhos, set(counts))

    ax = axes[2]
    if arms:
        irhos = sorted(arms)
        vals = []
        for col, (label, style) in INGEST_STYLES.items():
            ys = [arms[r][col] for r in irhos]
            vals += ys
            ax.plot(irhos, ys, markersize=2.5, linewidth=1.0, label=label,
                    zorder=3, **style)
        lo, hi = min(vals + [1.0]), max(vals)
        ax.set_ylim(lo - 0.15 * (hi - lo), hi + 0.15 * (hi - lo))
    else:
        print("  no ingest summary found, leaving the third panel empty")
    ax.set_ylabel("Ingest cost relative\nto No Index")

    for ax in axes:
        style_rho_axis(ax, rhos, xlabel=False)
    fig.supxlabel(r"$\rho$  (coarse $\rightarrow$ fine)", fontsize=6,
                  y=0.035)
    # Legends on one row above the panels: c over the two query panels, the
    # ingest costs over their own.
    b0, b1, b2 = (ax.get_position() for ax in axes)
    handles, labels = axes[0].get_legend_handles_labels()
    c_legend(fig, handles, labels, loc="lower center",
             bbox_to_anchor=((b0.x0 + b1.x1) / 2, b0.y1 + 0.005))
    if arms:
        handles, labels = axes[2].get_legend_handles_labels()
        c_legend(fig, handles, labels, loc="lower center",
                 bbox_to_anchor=((b2.x0 + b2.x1) / 2, b2.y1 + 0.005))
    save(fig, out_dir, "rho_sensitivity.pdf")


def main():
    parser = argparse.ArgumentParser(
        description="Plot the rho sensitivity experiment")
    parser.add_argument("summary_dir",
                        help="Directory holding summarize_rho_sensitivity.py "
                             "output (by default the read result directory)")
    parser.add_argument("-o", "--output-dir", default=None,
                        help="Where to write the PDF (default: summary_dir)")
    parser.add_argument("--stat", choices=["median", "mean"], default="median",
                        help="Per-cell statistic for the latency curves")
    args = parser.parse_args()

    out_dir = args.output_dir or args.summary_dir
    os.makedirs(out_dir, exist_ok=True)

    cells = load_query_cells(args.summary_dir)
    arms, _ = load_ingest(args.summary_dir)
    print(f"read cells: {len(cells)} "
          f"(c = {sorted({c for c, _ in cells})}, "
          f"{len({r for _, r in cells})} rho values); "
          f"ingest arms: {len(arms)}")
    plot_row(cells, arms, out_dir, args.stat)


if __name__ == "__main__":
    main()
