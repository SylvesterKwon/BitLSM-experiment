#!/usr/bin/env python3
"""Per-query cold latency, one row per index configuration.

What this figure is for: the ordering between index schemes -- per-template
composite, bitmap, conventional per-column secondary indexes -- not the cost
of any one query. So the queries carry no order and no labels. Lining them up
on an axis, as a grouped bar chart does, invites comparing one query against
another, which is meaningless: they have different intrinsic cost, and the
same query appears in every row.

Every query is a dot. At 13-22 queries that costs nothing and settles the
question an aggregate always raises -- whether one query is carrying the mean.
The geometric mean is a rule rather than a marker, deliberately a different
mark from the data it summarises, the way a forest plot separates its summary
from the studies.

Latency is on the horizontal axis and configurations are rows: this is the
orientation the form is normally drawn in (Cleveland dot plot, forest plot),
and a range of four orders of magnitude needs the figure's wide dimension.
Per-query numbers, for the cases where a reader wants one, are in
query_perf_summary_by_query.csv beside the sweep.

Usage:
    python3 experiments/myrocks_integration_test/plot_query_strip.py <result_dir> [<result_dir> ...]
    python3 experiments/myrocks_integration_test/plot_query_strip.py <dir> -o figs/ --format both
"""

import argparse
import csv
import os
import random
import statistics as st
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt                                  # noqa: E402

plt.rcParams.update({"font.size": 6})

# Grouped by storage engine, B-tree before LSM, and inside an engine the
# per-template composite (the upper bound) before the conventional per-column
# secondary indexes (the baseline). BitLSM is not a third engine -- it is
# MyRocks carrying a bitmap index -- so it sits inside the LSM block.
#
# The last row is the claim: sk_bi_v1 is the two rows above it combined, a
# bitmap added on top of the same conventional SKs, so the composition reads
# straight down the figure and the configuration being argued for ends it.
# (cell key, engine qualifier, index scheme, colour). The qualifier is empty
# for the LSM rows -- they are the comparison the section is about, so they
# read as plain index schemes; InnoDB is annotated rather than assumed.
#
# Paired by index scheme, B-tree first within a pair. That buys both
# adjacencies at once: the engine comparison is rows 1-2 and 3-4, and putting
# InnoDB first in each pair leaves "Single" directly above "Single + BitLSM",
# which is the pair the section argues. Ordering the pairs the other way would
# push InnoDB between them and break that.
#
# Read without the InnoDB rows, the sequence is the ingestion table's:
# Composite, Single, the bitmap on top of Single, the bitmap alone.
ROWS = [
    (("innodb", "composite_v1", "force_composite"),
     "InnoDB", "Composite", "#6A5A8A"),
    (("myrocks", "composite_v1", "force_composite"),
     "", "Composite", "#2E8B57"),
    (("innodb", "sk_v1", "auto"), "InnoDB", "Single", "#9888B8"),
    (("myrocks", "sk_v1", "auto"), "", "Single", "#4CC850"),
    (("bitlsm", "sk_bi_v1", "auto"), "", "Single + BitLSM", "#9B1B1B"),
    (("bitlsm", "bi_v1", "auto"), "", "BitLSM", "#E04040"),
]

JITTER = 0.10
JITTER_SEED = 1234      # fixed so a re-run redraws the same figure

FIG_W = 7.0             # two-column figure width in inches
FIG_H = FIG_W * 0.618 * 0.66 * 0.7


def load(result_dir):
    """{(engine, layout, plan): {query: cold_ms}}, the workload, and cold_reps."""
    path = os.path.join(result_dir, "perf_mode.csv")
    if not os.path.isfile(path):
        sys.exit(f"No perf_mode.csv in {result_dir}")
    cells, workload, reps = {}, None, 1
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            # session_vars is part of a cell's identity: two cells can differ
            # in nothing else. Only the unhinted ones belong in this figure.
            if not r["cold_ms"] or r["session_vars"]:
                continue
            workload = r["workload"]
            reps = max(reps, int(r.get("cold_reps") or 1))
            key = (r["engine"], r["index_layout"], r["plan"])
            # Seconds: the axis spans five orders of magnitude either way,
            # and seconds is the unit the numbers get quoted in.
            cells.setdefault(key, {})[r["query_id"]] = float(r["cold_ms"]) / 1000
    return cells, workload, reps


def dataset_name(workload):
    """Display name for a panel title. The internal identity carries a source
    prefix and a scale factor; neither belongs on the panel, where the name of
    the workload is the whole point. Scale is a property of the setup and is
    stated once in the text, not repeated on every figure."""
    if workload.startswith("pbi_"):
        return "Taxpayer"
    if workload.startswith("ssbflat"):
        return "SSB-flat"
    return workload


def fmt(v):
    if v < 0.01:
        return f"{v:.4f}"
    return f"{v:.3f}" if v < 10 else f"{v:.2f}"


def panel(ax, cells, queries):
    present = [(k, eng, sch, col) for k, eng, sch, col in ROWS if k in cells]
    for i, (key, engine, scheme, colour) in enumerate(present):
        y = len(present) - 1 - i
        vals = [cells[key][q] for q in queries]
        # A rule along the row, in the row's own colour: it ties a row's dots
        # together at this height and keeps the eye on one configuration
        # across the panel. Not a grid -- there is nothing to read off it.
        ax.axhline(y, color=colour, lw=0.5, alpha=0.22, zorder=1)
        rng = random.Random(JITTER_SEED)
        ax.scatter(vals, [y + rng.uniform(-JITTER, JITTER) for _ in vals],
                   s=4.5, c=colour, alpha=0.55, edgecolors="none", zorder=3)
        gm = st.geometric_mean(vals)
        # A short heavy rule, not a marker: it pins the summary to one x
        # without covering the dots it summarises, and cannot be mistaken for
        # another query.
        ax.plot([gm, gm], [y - 0.17, y + 0.17], color=colour, lw=2.8,
                solid_capstyle="butt", zorder=5)
        ax.annotate(fmt(gm), (gm, y), textcoords="offset points",
                    xytext=(0, 5), ha="center", fontsize=5,
                    color=colour, zorder=6)
    ax.set_xscale("log")
    ax.set_ylim(-0.6, len(present) - 0.25)
    ax.grid(False)
    # The log x axis carries minor ticks too; they default to pointing
    # out, which leaves them sticking through the frame.
    ax.tick_params(which="major", length=2, width=0.3, direction="in")
    ax.tick_params(which="minor", length=1.2, width=0.3, direction="in")
    for sp in ax.spines.values():
        sp.set_linewidth(0.5)
    return present


def plot(result_dirs, out_dir, fmts, title):
    fig, axes = plt.subplots(1, len(result_dirs),
                             figsize=(FIG_W, FIG_H),
                             sharey=True, squeeze=False)
    present = []
    for ax, d in zip(axes[0], result_dirs):
        cells, workload, reps = load(d)
        missing = [k for k, _, _, _ in ROWS if k not in cells]
        # Every row must plot the same queries or the geometric means are
        # not comparable. The intersection guarantees that; saying so out
        # loud is what stops a cell quietly shrinking the panel.
        sets = [set(v) for v in cells.values()]
        queries = sorted(set.intersection(*sets))
        dropped = sorted(set.union(*sets) - set(queries))
        if dropped:
            print(f"WARNING: {d} drops {', '.join(dropped)} — not every "
                  f"cell measured them")
        present = panel(ax, cells, queries)
        ax.set_title(dataset_name(workload), fontsize=6.5)
        # The panel carries the dataset name only; the rest of the subcaption
        # is set in LaTeX, and is printed here so it cannot drift from the
        # data it describes.
        tag = (f"({chr(97 + list(axes[0]).index(ax))}) "
               f"{dataset_name(workload)}, n={len(queries)}")
        if reps > 1:
            tag += f", median of {reps} cold runs"
        print(f"caption: {tag}")
        if missing:
            print(f"note: {d} has no "
                  f"{', '.join('/'.join(m) for m in missing)} cell")
    axes[0][0].set_yticks(range(len(present)))
    # BitLSM is the bitmap index; naming the scheme again on its own row
    # would only repeat the engine name.
    axes[0][0].set_yticklabels(
        [f"{sch} ({eng})" if eng else sch
         for _, eng, sch, _ in present][::-1], fontsize=6)
    if title:
        fig.suptitle(title, fontsize=7)

    # One axis label for both panels: they share the quantity and the scale.
    fig.supxlabel("Query Latency (s)", fontsize=6, y=0.045)
    fig.subplots_adjust(left=0.135, right=0.995, bottom=0.175, top=0.88,
                        wspace=0.10)
    os.makedirs(out_dir, exist_ok=True)
    stem = os.path.join(out_dir, "query_strip")
    for f in fmts:
        path = f"{stem}.{f}"
        # The engine column sits outside the axes, so tight bbox needs a
        # little padding or it trims the labels.
        fig.savefig(path, dpi=150)
        print(f"Saved: {path}")
    plt.close(fig)


def main():
    p = argparse.ArgumentParser(
        description="Per-query cold latency, one row per configuration")
    p.add_argument("result_dir", nargs="+")
    p.add_argument("-o", "--output-dir", default=None)
    p.add_argument("--title", default=None)
    p.add_argument("--format", default="pdf",
                   choices=["pdf", "png", "both"])
    a = p.parse_args()
    out = a.output_dir or a.result_dir[0]
    fmts = ["pdf", "png"] if a.format == "both" else [a.format]
    plot(a.result_dir, out, fmts, a.title)


if __name__ == "__main__":
    main()
