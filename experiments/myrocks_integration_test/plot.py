#!/usr/bin/env python3
"""Plot ingest time and on-disk size per cell for myrocks_integration_test.

Reads the `ingest.csv` written by ingest_run.py and draws one bar per
(engine, index_layout) cell in two panels: wall-clock seconds to stream the
whole table row-by-row, and the resulting datadir size.

Ingest time spans ~45x between the LSM cells and InnoDB's composite layout. A
plain linear axis flattens the five LSM bars into a rug, so the outliers are
truncated at a cap and marked with a wavy axis break, their true value printed
above the bar. Pass --log for a log axis instead, or --cap to set the cut by
hand. Size spans only ~2.6x and never needs breaking.
"""

import argparse
import csv
import os

import matplotlib.pyplot as plt
import numpy as np

plt.rcParams.update({"font.size": 6})


# (engine, index_layout) in plot order: grouped by engine family, PK-only
# control first within each group so the maintenance cost reads left to right.
CELL_ORDER = [
    ("myrocks", "std"),
    ("myrocks", "sk_v1"),
    ("myrocks", "composite_v1"),
    ("bitlsm", "bi_v1"),
    ("bitlsm", "sk_bi_v1"),
    ("innodb", "std"),
    ("innodb", "sk_v1"),
    ("innodb", "composite_v1"),
]
# Engine is carried by colour + legend, so the tick labels name only the
# index layout -- spelling out the engine here overruns the tick spacing.
# The layout names are the paper's, capitalised: they are proper nouns for the
# configurations under comparison, and the read figure spells them the same way.
CELL_LABELS = {
    ("myrocks", "std"): "PK only",
    ("myrocks", "sk_v1"): "Single",
    ("myrocks", "composite_v1"): "Composite",
    ("bitlsm", "bi_v1"): "BitLSM",
    ("bitlsm", "sk_bi_v1"): "Single\n+ BitLSM",
    ("innodb", "std"): "PK only",
    ("innodb", "sk_v1"): "Single",
    ("innodb", "composite_v1"): "Composite",
}
CELL_COLORS = {
    ("myrocks", "std"): "#A8D8A0",
    ("myrocks", "sk_v1"): "#4CC850",
    ("myrocks", "composite_v1"): "#2E8B57",
    ("bitlsm", "bi_v1"): "#E04040",
    ("bitlsm", "sk_bi_v1"): "#9B1B1B",
    ("innodb", "std"): "#C8BCE0",
    ("innodb", "sk_v1"): "#9888B8",
    ("innodb", "composite_v1"): "#6A5A8A",
}
ENGINE_LEGEND = [
    ("myrocks", "MyRocks", "#4CC850"),
    ("bitlsm", "BitLSM", "#E04040"),
    ("innodb", "InnoDB", "#9888B8"),
]

GIB = float(1 << 30)

# Values above OUTLIER_K * median are treated as outliers when picking an
# automatic cap; the cap then clears the tallest bar that is not one of them.
OUTLIER_K = 4.0
CAP_HEADROOM = 1.25


def load_ingest(result_dir: str):
    """Return {(engine, index_layout): {"seconds": float, "gib": float}}."""
    csv_path = os.path.join(result_dir, "ingest.csv")
    data: dict[tuple[str, str], dict[str, float]] = {}
    with open(csv_path, newline="") as f:
        for row in csv.DictReader(f):
            key = (row["engine"].strip(), row["index_layout"].strip())
            try:
                data[key] = {
                    "seconds": float(row["seconds"]),
                    "gib": int(row["db_bytes"]) / GIB,
                }
            except (KeyError, ValueError):
                continue
    return data


def ordered_cells(data_keys):
    ordered = [c for c in CELL_ORDER if c in data_keys]
    return ordered + sorted(set(data_keys) - set(CELL_ORDER))


def label_for(cell):
    return CELL_LABELS.get(cell, "/".join(cell))


def auto_cap(values):
    """Cap that clears every non-outlier bar, or None if nothing stands out."""
    med = float(np.median(values))
    inliers = [v for v in values if v <= OUTLIER_K * med]
    if not inliers or len(inliers) == len(values):
        return None
    return max(inliers) * CAP_HEADROOM


def draw_axis_break(ax, xc, width, y, height, waves=4):
    """Wavy white band across a truncated bar, drawn at data coords y."""
    pad = width * 0.06
    xs = np.linspace(xc - width / 2 - pad, xc + width / 2 + pad, 160)
    phase = np.linspace(0, waves * np.pi, xs.size)
    wave = np.sin(phase) * height * 0.45
    lower, upper = y - height / 2 + wave, y + height / 2 + wave
    ax.fill_between(xs, lower, upper, color="white", zorder=3, linewidth=0)
    for edge in (lower, upper):
        ax.plot(xs, edge, color="black", linewidth=0.5, zorder=4,
                solid_capstyle="round")


def draw_panel(ax, cells, values, ylabel, fmt, log=False, cap=None):
    x = np.arange(len(cells))
    colors = [CELL_COLORS.get(c, "#999999") for c in cells]
    width = 0.68

    if log:
        heights, broken = values, [False] * len(values)
    else:
        broken = [cap is not None and v > cap for v in values]
        heights = [cap if b else v for b, v in zip(broken, values)]

    bars = ax.bar(x, heights, width, color=colors, zorder=2)

    if log:
        ax.set_yscale("log")
        ax.set_ylim(min(values) / 3.0, max(values) * 3.0)
    else:
        ax.set_ylim(0, max(heights) * 1.16)

    for bar, v, b in zip(bars, values, broken):
        top = bar.get_height()
        if b:
            draw_axis_break(ax, bar.get_x() + width / 2, width,
                            top * 0.88, top * 0.05)
        ax.text(bar.get_x() + width / 2,
                top * (1.06 if log else 1.0) + (0 if log else max(heights) * 0.015),
                fmt(v), ha="center", va="bottom", zorder=5)

    ax.set_ylabel(ylabel)
    ax.set_xticks(x)
    ax.set_xticklabels([label_for(c) for c in cells], fontsize=5.5)
    ax.tick_params(axis="x", length=0)
    ax.tick_params(axis="y", length=2, width=0.3, direction="in")
    ax.grid(False)


def plot_ingest(data, output_dir, log, cap, fmts):
    cells = ordered_cells(data.keys())
    if not cells:
        print("No ingest rows found.")
        return

    seconds = [data[c]["seconds"] for c in cells]
    gib = [data[c]["gib"] for c in cells]
    if not log and cap is None:
        cap = auto_cap(seconds)

    fig, axes = plt.subplots(1, 2, figsize=(7, 7 * 0.618 * 0.66))

    draw_panel(axes[0], cells, seconds, "Ingest time (s)",
               lambda v: f"{v:,.0f}", log=log, cap=cap)
    draw_panel(axes[1], cells, gib, "Datadir size (GiB)",
               lambda v: f"{v:.2f}")

    # The legend lists only the engines present in the data.
    present = {engine for engine, _ in cells}
    legend = [(n, c) for e, n, c in ENGINE_LEGEND if e in present]
    handles = [plt.Rectangle((0, 0), 1, 1, color=c) for _, c in legend]
    axes[0].legend(handles, [n for n, _ in legend],
                   loc="upper left", frameon=False, ncol=3,
                   handlelength=1.0, columnspacing=1.0)

    fig.tight_layout()

    for ext in fmts:
        out_path = os.path.join(output_dir, f"ingest_walltime_size.{ext}")
        fig.savefig(out_path, dpi=150)
        print(f"Saved: {out_path}")
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(
        description="Plot ingest time and datadir size per cell")
    parser.add_argument("result_dir", help="Result directory holding ingest.csv")
    parser.add_argument("-o", "--output-dir", default=None,
                        help="Output directory (default: result_dir)")
    parser.add_argument("--log", action="store_true",
                        help="Log ingest-time axis instead of a broken linear one")
    parser.add_argument("--cap", type=float, default=None,
                        help="Truncate ingest-time bars above this many seconds "
                             "(default: derived from the data)")
    parser.add_argument("--format", default="pdf",
                        choices=["pdf", "png", "both"],
                        help="Output format (default: pdf)")
    args = parser.parse_args()

    output_dir = args.output_dir or args.result_dir
    os.makedirs(output_dir, exist_ok=True)

    fmts = ["pdf", "png"] if args.format == "both" else [args.format]
    plot_ingest(load_ingest(args.result_dir), output_dir,
                log=args.log, cap=args.cap, fmts=fmts)


if __name__ == "__main__":
    main()
