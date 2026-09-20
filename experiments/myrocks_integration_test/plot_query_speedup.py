#!/usr/bin/env python3
"""Per-query speedup over conventional secondary indexes, one bar per cell.

The form follows what this literature uses for a fixed benchmark of 13-22
queries: every query gets its own group of bars, the geometric mean is
appended as a final group rather than replacing the per-query detail, and a
ratio axis is drawn on a log scale with an explicit 1x line.

Ratio, not absolute latency, for a reason the axis forces: cold latencies here
span four orders of magnitude within one workload, and absolute numbers in
this literature are always drawn on a linear axis, which that range makes
unreadable. Ratios compress to a range a log axis reads well, and the paired
design makes them the honest statistic anyway -- every cell runs the same
query, so dividing removes the query's own difficulty.

The baseline is the same engine carrying conventional per-column secondary
indexes: the configuration you would build without a bitmap index, holding
engine, data and server settings fixed so the index scheme is the only
variable.

Under each query, its selectivity (matching rows / table rows) from the cached
ground truth. Bars that run off the top are clipped with their true value
printed above, which is what most papers here do with an outlier.

Usage:
    python3 experiments/myrocks_integration_test/plot_query_speedup.py <result_dir> [<result_dir> ...]
    python3 experiments/myrocks_integration_test/plot_query_speedup.py <dir> -o figs/ --format both
"""

import argparse
import csv
import glob
import json
import os
import statistics as st
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt                                  # noqa: E402
from matplotlib.gridspec import GridSpec                         # noqa: E402
from matplotlib.patches import Patch                             # noqa: E402

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "src"))

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
plt.rcParams.update({"font.size": 6})

BASELINE = ("myrocks", "sk_v1", "auto")
CELLS = [
    (("bitlsm", "bi_v1", "auto"), "BitLSM: bi", "#E04040"),
    (("bitlsm", "sk_bi_v1", "auto"), "BitLSM: SK + bi", "#9B1B1B"),
    (("myrocks", "composite_v1", "force_composite"),
     "MyRocks: composite", "#2E8B57"),
    (("innodb", "sk_v1", "auto"), "InnoDB: SK", "#9888B8"),
    (("innodb", "composite_v1", "force_composite"),
     "InnoDB: composite", "#6A5A8A"),
]

# A bar this far above the bulk is clipped and labelled rather than allowed to
# set the axis for everything else.
CLIP_PERCENTILE = 0.92
CLIP_HEADROOM = 1.6


def load(result_dir):
    """{(engine, layout, plan): {query: cold_ms}}, the workload, and cold_reps."""
    path = os.path.join(result_dir, "perf_mode.csv")
    if not os.path.isfile(path):
        sys.exit(f"No perf_mode.csv in {result_dir}")
    cells, workload, reps = {}, None, 1
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            if not r["cold_ms"] or r["session_vars"]:
                continue
            workload = r["workload"]
            reps = max(reps, int(r.get("cold_reps") or 1))
            key = (r["engine"], r["index_layout"], r["plan"])
            cells.setdefault(key, {})[r["query_id"]] = float(r["cold_ms"])
    return cells, workload, reps


def selectivities(workload):
    hits = glob.glob(os.path.join(REPO, "data", "sql", "**",
                                  f"ground_truth.{workload}.json"),
                     recursive=True)
    if not hits:
        return {}
    counts = json.load(open(hits[0]))
    try:
        from myrocks.workloads.registry import make_workload
        sf = int(workload.rsplit("sf", 1)[1]) if "sf" in workload else None
        cfg = {"workload": "ssbflat" if sf else "pbi_taxpayer"}
        if sf:
            cfg["sf"] = sf
        w = make_workload(cfg)
        total = w.EXPECTED_TOTAL_ROWS
        if not total:
            # Generated workloads only carry the canonical count at SF1; the
            # sidecar written beside the generated file has the real one.
            meta = getattr(w, "flat_path", "") + ".meta.json"
            if os.path.isfile(meta):
                total = json.load(open(meta)).get("rows")
    except Exception:
        return {}
    return {q: c / total for q, c in counts.items()} if total else {}


def short(query_id):
    """ssbflat_q1_1 -> Q1.1 ; pbitax_q01 -> Q01"""
    return "Q" + query_id.rsplit("_q", 1)[-1].replace("_", ".")


def clip_at(values):
    """Where to cut the axis, or None when nothing runs away from the bulk."""
    vals = sorted(values)
    if len(vals) < 6:
        return None
    bulk = vals[int(len(vals) * CLIP_PERCENTILE)]
    cap = bulk * CLIP_HEADROOM
    return cap if max(vals) > cap * 1.5 else None


def panel(ax, cells, queries, sel, ylabel):
    present = [(k, lab, col) for k, lab, col in CELLS if k in cells]
    base = cells[BASELINE]
    speed = {k: {q: base[q] / cells[k][q] for q in queries if q in cells[k]}
             for k, _, _ in present}
    gm = {k: st.geometric_mean(list(v.values())) for k, v in speed.items()}

    slots = list(queries) + ["GM"]
    n = len(present)
    width = 0.8 / n
    cap = clip_at([v for d in speed.values() for v in d.values()]
                  + list(gm.values()))

    for i, (key, label, colour) in enumerate(present):
        xs, ys, over = [], [], []
        for j, s in enumerate(slots):
            v = gm[key] if s == "GM" else speed[key].get(s)
            if v is None:
                continue
            x = j - 0.4 + width * (i + 0.5)
            xs.append(x)
            if cap and v > cap:
                ys.append(cap)
                over.append((x, v))
            else:
                ys.append(v)
        ax.bar(xs, ys, width=width, color=colour, label=label, linewidth=0,
               zorder=3)
        for x, v in over:
            # Adjacent clipped bars would stack their labels on top of each
            # other; step them by series so all stay readable.
            ax.annotate(f"{v:,.0f}x", (x, cap), textcoords="offset points",
                        xytext=(0, 2 + 11 * i), ha="center", fontsize=4.2,
                        color=colour, rotation=90, zorder=6)

    ax.axhline(1.0, color="#333", lw=0.7, ls="--", zorder=4)
    ax.set_yscale("log")
    if cap:
        ax.set_ylim(top=cap * 1.02)
        ax.margins(y=0.28)
    # The GM group is a summary, not another query: separate it with a rule.
    ax.axvline(len(queries) - 0.5, color="#999", lw=0.5, zorder=2)
    ax.set_xlim(-0.6, len(slots) - 0.4)
    ax.set_xticks(range(len(slots)))
    labels = []
    for s in slots:
        if s == "GM":
            labels.append("GM")
            continue
        v = sel.get(s)
        labels.append(f"{short(s)}\n{v:.0e}" if v else short(s))
    ax.set_xticklabels(labels, fontsize=4.8)
    ax.grid(axis="y", lw=0.3, alpha=0.35)
    ax.set_axisbelow(True)
    ax.tick_params(length=2, width=0.4)
    for sp in ax.spines.values():
        sp.set_linewidth(0.5)
    if ylabel:
        ax.set_ylabel(ylabel)
    return present


def plot(result_dirs, out_dir, fmts, title):
    loaded = []
    for d in result_dirs:
        cells, workload, reps = load(d)
        if BASELINE not in cells:
            sys.exit(f"{d}: no {BASELINE} cell to normalise against")
        queries = sorted(set(cells[BASELINE]))
        loaded.append((cells, workload, reps, queries, selectivities(workload)))

    widths = [len(x[3]) + 1 for x in loaded]
    fig = plt.figure(figsize=(sum(widths) * 0.30 + 1.0, 2.2))
    gs = GridSpec(1, len(loaded), figure=fig, width_ratios=widths, wspace=0.16)

    present = []
    for col, (cells, workload, reps, queries, sel) in enumerate(loaded):
        ax = fig.add_subplot(gs[0, col])
        present = panel(ax, cells, queries, sel,
                        "Speedup over MyRocks SK" if col == 0 else "")
        tag = f"({chr(97 + col)}) {workload}"
        if reps > 1:
            tag += f", median of {reps}"
        ax.set_xlabel(f"Query / selectivity\n{tag}", fontsize=6)

    handles = [Patch(facecolor=c, label=l) for _, l, c in present]
    fig.legend(handles=handles, loc="upper center", ncol=len(handles),
               frameon=False, fontsize=6, handlelength=1.0,
               columnspacing=1.4, bbox_to_anchor=(0.5, 1.10))
    if title:
        fig.suptitle(title, y=1.20, fontsize=7)

    os.makedirs(out_dir, exist_ok=True)
    stem = os.path.join(out_dir, "query_speedup")
    for fmt in fmts:
        path = f"{stem}.{fmt}"
        fig.savefig(path, bbox_inches="tight", dpi=200)
        print(f"Saved: {path}")
    plt.close(fig)


def main():
    p = argparse.ArgumentParser(
        description="Per-query speedup over conventional secondary indexes")
    p.add_argument("result_dir", nargs="+")
    p.add_argument("-o", "--output-dir", default=None)
    p.add_argument("--only", default=None,
                   help="Comma-separated cell labels to keep")
    p.add_argument("--title", default=None)
    p.add_argument("--format", default="pdf",
                   choices=["pdf", "png", "both"])
    a = p.parse_args()
    if a.only:
        keep = {x.strip() for x in a.only.split(",")}
        CELLS[:] = [c for c in CELLS if c[1] in keep]
    out = a.output_dir or a.result_dir[0]
    fmts = ["pdf", "png"] if a.format == "both" else [a.format]
    plot(a.result_dir, out, fmts, a.title)


if __name__ == "__main__":
    main()
