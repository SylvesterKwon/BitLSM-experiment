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
Per-query detail in the conventional grouped-bar form is plot_query_speedup.py.

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

plt.rcParams.update({"font.size": 7})

# Ordered by index family so the three tiers read top to bottom. The row label
# names engine and layout outright -- with configurations on an axis of their
# own there is room for it, and it saves the reader a legend.
ROWS = [
    (("innodb", "composite_v1", "force_composite"), "InnoDB  comp", "#6A5A8A"),
    (("myrocks", "composite_v1", "force_composite"), "MyRocks comp", "#2E8B57"),
    (("bitlsm", "sk_bi_v1", "auto"), "BitLSM  SK+bi", "#9B1B1B"),
    (("bitlsm", "bi_v1", "auto"), "BitLSM  bi", "#E04040"),
    (("innodb", "sk_v1", "auto"), "InnoDB  SK", "#9888B8"),
    (("myrocks", "sk_v1", "auto"), "MyRocks SK", "#4CC850"),
]

JITTER = 0.15
JITTER_SEED = 1234      # fixed so a re-run redraws the same figure


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
            cells.setdefault(key, {})[r["query_id"]] = float(r["cold_ms"])
    return cells, workload, reps


def fmt(v):
    return f"{v:.1f}" if v < 10 else f"{v:,.0f}"


def panel(ax, cells, queries):
    present = [(k, lab, col) for k, lab, col in ROWS if k in cells]
    for i, (key, label, colour) in enumerate(present):
        y = len(present) - 1 - i
        vals = [cells[key][q] for q in queries]
        rng = random.Random(JITTER_SEED)
        ax.scatter(vals, [y + rng.uniform(-JITTER, JITTER) for _ in vals],
                   s=9, c=colour, alpha=0.55, edgecolors="none", zorder=3)
        gm = st.geometric_mean(vals)
        # A short heavy rule, not a marker: it pins the summary to one x
        # without covering the dots it summarises, and cannot be mistaken for
        # another query.
        ax.plot([gm, gm], [y - 0.17, y + 0.17], color=colour, lw=2.8,
                solid_capstyle="butt", zorder=5)
        ax.annotate(fmt(gm), (gm, y), textcoords="offset points",
                    xytext=(0, 8), ha="center", fontsize=5.5,
                    color=colour, zorder=6)
    ax.set_xscale("log")
    ax.set_ylim(-0.6, len(present) - 0.25)
    ax.grid(axis="x", lw=0.3, alpha=0.35)
    ax.set_axisbelow(True)
    ax.tick_params(length=2, width=0.4)
    for sp in ax.spines.values():
        sp.set_linewidth(0.5)
    return present


def plot(result_dirs, out_dir, fmts, title):
    fig, axes = plt.subplots(1, len(result_dirs),
                             figsize=(3.6 * len(result_dirs), 3.0),
                             sharey=True, squeeze=False)
    present = []
    for ax, d in zip(axes[0], result_dirs):
        cells, workload, reps = load(d)
        missing = [k for k, _, _ in ROWS if k not in cells]
        queries = sorted(set.intersection(*[set(v) for v in cells.values()]))
        present = panel(ax, cells, queries)
        tag = f"({chr(97 + list(axes[0]).index(ax))}) {workload}  n={len(queries)}"
        if reps > 1:
            tag += f", median of {reps}"
        ax.set_title(tag, fontsize=7)
        ax.set_xlabel("Cold latency per query (ms, log)")
        if missing:
            print(f"note: {d} has no "
                  f"{', '.join('/'.join(m) for m in missing)} cell")
    axes[0][0].set_yticks(range(len(present)))
    axes[0][0].set_yticklabels([l for _, l, _ in present][::-1], fontsize=7)
    if title:
        fig.suptitle(title, fontsize=8)

    os.makedirs(out_dir, exist_ok=True)
    stem = os.path.join(out_dir, "query_strip")
    for f in fmts:
        path = f"{stem}.{f}"
        fig.savefig(path, bbox_inches="tight", dpi=200)
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
