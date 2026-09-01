#!/usr/bin/env python3
"""Plot query latency per cell for myrocks_integration_test perf-mode runs.

Reads the `perf_mode.csv` written by perf_run.py and totals cold and warm
latency over the queries every cell has in common -- the composite cells skip
any query with no designed composite, so summing each cell over its own query
set would compare different workloads.

By default only the configuration each cell would actually be deployed in is
drawn (`auto`, or `force_composite` where the cell has no unhinted plan),
plus `ignore_bi` as the paired control that turns the bitmap off inside the
same datadir. --all-plans draws every hinted variant as well.

Latency spans two orders of magnitude on taxpayer, so outliers are truncated
at a cap and marked with a wavy axis break, their true value printed above
the bar; --log switches to a log axis and --cap sets the cut by hand.
"""

import argparse
import csv
import os

import matplotlib.pyplot as plt
import numpy as np

plt.rcParams.update({"font.size": 6})


# (engine, index_layout, session_vars_present, plan) -> label, colour.
# Ordered so the BitLSM cells sit next to the baseline they are argued against.
# Engine is carried by colour + legend, so the tick labels name only the
# index layout -- spelling out the engine here overruns the tick spacing.
PRIMARY = [
    (("bitlsm", "bi_v1", False, "auto"), "bi", "#E04040"),
    (("bitlsm", "sk_bi_v1", False, "auto"), "SK + bi", "#9B1B1B"),
    (("myrocks", "sk_v1", False, "auto"), "+ SK", "#4CC850"),
    (("myrocks", "composite_v1", False, "force_composite"),
     "+ comp", "#2E8B57"),
    (("innodb", "sk_v1", False, "auto"), "+ SK", "#9888B8"),
    (("innodb", "composite_v1", False, "force_composite"),
     "+ comp", "#6A5A8A"),
]
EXTRA = [
    (("bitlsm", "bi_v1", False, "force_bi"), "bi\nforced", "#F0A0A0"),
    (("bitlsm", "sk_bi_v1", False, "force_bi"), "SK + bi\nforced", "#B85050"),
    (("myrocks", "sk_v1", False, "index_merge"), "+ SK\nidx mrg", "#A8D8A0"),
    (("innodb", "sk_v1", False, "index_merge"), "+ SK\nidx mrg", "#B0A4C8"),
]

ENGINE_LEGEND = [("BitLSM", "#9B1B1B"), ("MyRocks", "#4CC850"),
                 ("InnoDB", "#9888B8")]

OUTLIER_K = 4.0
CAP_HEADROOM = 1.25


def load_perf(result_dir: str):
    """{cell_key: {query_id: (cold_ms, warm_ms)}} from perf_mode.csv."""
    path = os.path.join(result_dir, "perf_mode.csv")
    out = {}
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            key = (r["engine"].strip(), r["index_layout"].strip(),
                   bool(r["session_vars"].strip()), r["plan"].strip())
            try:
                out.setdefault(key, {})[r["query_id"]] = (
                    float(r["cold_ms"]), float(r["warm_ms_median"]))
            except (KeyError, ValueError):
                continue
    return out


def common_queries(data):
    sets = [set(q) for q in data.values() if q]
    return set.intersection(*sets) if sets else set()


def auto_cap(values):
    med = float(np.median(values))
    inliers = [v for v in values if v <= OUTLIER_K * med]
    if not inliers or len(inliers) == len(values):
        return None
    return max(inliers) * CAP_HEADROOM


def draw_axis_break(ax, xc, width, y, height, waves=4):
    pad = width * 0.06
    xs = np.linspace(xc - width / 2 - pad, xc + width / 2 + pad, 160)
    wave = np.sin(np.linspace(0, waves * np.pi, xs.size)) * height * 0.45
    lower, upper = y - height / 2 + wave, y + height / 2 + wave
    ax.fill_between(xs, lower, upper, color="white", zorder=3, linewidth=0)
    for edge in (lower, upper):
        ax.plot(xs, edge, color="black", linewidth=0.5, zorder=4,
                solid_capstyle="round")


def scale_for(values):
    """Plot in seconds once milliseconds stop being readable on the axis."""
    if max(values) >= 10000:
        return 1000.0, "s", lambda v: f"{v:,.0f}"
    return 1.0, "ms", lambda v: f"{v:,.0f}"


def draw_panel(ax, labels, colors, values, ylabel, log, cap, fmt):
    x = np.arange(len(values))
    width = 0.68
    if log:
        heights, broken = values, [False] * len(values)
    else:
        broken = [cap is not None and v > cap for v in values]
        heights = [cap if b else v for b, v in zip(broken, values)]

    bars = ax.bar(x, heights, width, color=colors, zorder=2)
    if log:
        ax.set_yscale("log")
        ax.set_ylim(min(v for v in values if v > 0) / 3.0, max(values) * 3.0)
    else:
        ax.set_ylim(0, max(heights) * 1.18)

    for bar, v, b in zip(bars, values, broken):
        top = bar.get_height()
        if b:
            draw_axis_break(ax, bar.get_x() + width / 2, width,
                            top * 0.88, top * 0.05)
        ax.text(bar.get_x() + width / 2,
                top * (1.06 if log else 1.0)
                + (0 if log else max(heights) * 0.015),
                fmt(v), ha="center", va="bottom", zorder=5)

    ax.set_ylabel(ylabel)
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.tick_params(axis="x", length=0)
    ax.tick_params(axis="y", length=2, width=0.3, direction="in")
    ax.grid(False)


def plot_perf(result_dir, output_dir, log, cap, fmts, all_plans, title):
    data = load_perf(result_dir)
    qs = common_queries(data)
    if not qs:
        print("No overlapping queries across cells.")
        return

    spec = PRIMARY + (EXTRA if all_plans else [])
    rows = [(lab, col, sum(data[k][q][0] for q in qs),
             sum(data[k][q][1] for q in qs))
            for k, lab, col in spec if k in data]
    if not rows:
        print("None of the expected cells are present.")
        return

    labels = [r[0] for r in rows]
    colors = [r[1] for r in rows]
    cold = [r[2] for r in rows]
    warm = [r[3] for r in rows]

    w = 7 if not all_plans else 9
    fig, axes = plt.subplots(1, 2, figsize=(w, w * 0.618 * 0.66))
    for ax, vals, kind in ((axes[0], cold, "Cold"), (axes[1], warm, "Warm")):
        div, unit, fmt = scale_for(vals)
        scaled = [v / div for v in vals]
        panel_cap = cap / div if cap is not None else (
            None if log else auto_cap(scaled))
        draw_panel(ax, labels, colors, scaled,
                   f"{kind} latency, {len(qs)} queries ({unit})",
                   log, panel_cap, fmt)

    handles = [plt.Rectangle((0, 0), 1, 1, color=c) for _, c in ENGINE_LEGEND]
    axes[0].legend(handles, [n for n, _ in ENGINE_LEGEND], loc="upper left",
                   frameon=False, ncol=3, handlelength=1.0, columnspacing=1.0)
    if title:
        fig.suptitle(title, fontsize=7)
    fig.tight_layout()

    stem = "perf_latency" + ("_all_plans" if all_plans else "")
    for ext in fmts:
        out = os.path.join(output_dir, f"{stem}.{ext}")
        fig.savefig(out, dpi=150)
        print(f"Saved: {out}")
    plt.close(fig)


def main():
    p = argparse.ArgumentParser(
        description="Plot query latency per cell from a perf-mode run")
    p.add_argument("result_dir", help="Result directory holding perf_mode.csv")
    p.add_argument("-o", "--output-dir", default=None)
    p.add_argument("--log", action="store_true",
                   help="Log latency axes instead of broken linear ones")
    p.add_argument("--cap", type=float, default=None,
                   help="Truncate bars above this many ms")
    p.add_argument("--all-plans", action="store_true",
                   help="Include the hinted plan variants too")
    p.add_argument("--title", default=None)
    p.add_argument("--format", default="pdf",
                   choices=["pdf", "png", "both"])
    a = p.parse_args()

    out_dir = a.output_dir or a.result_dir
    os.makedirs(out_dir, exist_ok=True)
    fmts = ["pdf", "png"] if a.format == "both" else [a.format]
    plot_perf(a.result_dir, out_dir, a.log, a.cap, fmts, a.all_plans, a.title)


if __name__ == "__main__":
    main()
