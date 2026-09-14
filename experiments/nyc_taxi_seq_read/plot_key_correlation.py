#!/usr/bin/env python3
"""Plot the ULID key-correlation subsection: latency against query selectivity.

One single-column panel. Every query of the interval_window workloads
(pickup >= x AND dropoff < y) is placed at its own selectivity -- rows matched
over rows in the DB -- the queries are cut into log-spaced selectivity bins,
and each bin's MEDIAN latency is drawn per method. Selectivity is the one
thing that varies across the 900 queries (one template, one DB), so the
binned medians form a curve, and the curve is the mechanism: BitLSM-Global
sits on a floor while the query selects fewer rows than one global bin holds
(it fetches the whole bin however narrow the window), BitLSM tracks the query
because its per-SST bins are minutes wide, and the two meet where the query
outgrows a global bin. Per-Block Filters lie on BitLSM throughout: block zone
maps prune fully once the predicate is on the key-correlated attribute.

Selectivity rather than window width is the axis so the floor reads against
rho: with equi-depth bins every global pickup bin holds exactly 1/bins of the
rows, and that fraction is the dotted vertical line, read from the bin policy
the BitLSM-Global DB was built with (1/1821 at rho = 0.001, the attr_num/rho
budget shared by the greedy over six attributes). The line carries no label in
the figure; the caption names it.

Inputs: the read_window result directory (key_correlation_queries.csv from
summarize_key_correlation.py) and the bin policy file.

Usage:
    python3 experiments/nyc_taxi_seq_read/plot_key_correlation.py <window_result_dir> \
        --bin_policy /scratch/honk/bin_policy/write_seq_2024-2025_all_ulid_rho0.001.bin \
        [-o <output_dir>]
"""

import argparse
import csv
import math
import os
import statistics
import struct
from collections import defaultdict

import matplotlib.pyplot as plt
import matplotlib.ticker as mticker

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

# Height/width of the plot box. The paper's grid cells are 0.702, but that was
# measured on 1.3-in-wide cells; a lone box spanning the column at 0.702 is
# 2 in tall, so this one is flatter by request. Pinned, with the figure height
# derived from the box plus the furniture it carries rather than left to
# tight_layout.
PANEL_BOX_ASPECT = 0.5
FIG_W = 3.333               # single text column, inches
LEFT, RIGHT = 0.135, 0.965  # room for the y label, and for the last x
                            # tick label ($10^{-2}$) to sit on the right edge
LEGEND_H = 0.20             # legend strip above the box
TICKS_H = 0.15              # x tick labels under the box (math text is taller)
XLABEL_H = 0.15             # x label under the ticks

# (method key as in the result file names, legend label, color, marker).
# Per-Block Filters and BitLSM keep nyc_taxi_seq_read/plot.py's colors;
# BitLSM-Global's orange is its own convention (CLAUDE.md figure style).
METHODS = [
    ("embedded_bloom_bits10", "Per-Block Filters", "#1FA8A0", "P"),
    ("bitlsm-global_rho0.001", "BitLSM-Global", "#E69F00", "D"),
    ("bitlsm_rho0.001", "BitLSM", "#9B1B1B", "o"),
]
START_ATTR = "tpep_pickup_datetime"   # the window's lower bound, and the key

# honk_player logs records_total as 0 for read workloads, so per-query
# selectivity is recovered against the known row count of the ingest.
TOTAL_ROWS = 89_892_322

# Selectivity bins: 12 log-spaced bins (four per decade) over the three
# workload bands, [1e-5, 1e-2). A bin holding fewer queries than
# MIN_BIN_QUERIES is not drawn -- its median would be one or two queries'
# noise.
SEL_BINS = 12
SEL_MIN, SEL_MAX = 1e-5, 1e-2
MIN_BIN_QUERIES = 10

QUERIES_CSV = "key_correlation_queries.csv"
POLICY_MAGIC = b"GBINPOL1"


def global_bin_count(policy_path, attr_names, attr):
    """Number of bins the policy gives `attr` (bin_policy.cpp layout: header,
    then per attribute [type u8][bins u32] and a BytesList of boundaries for
    range attributes or (value, bin) entries otherwise)."""
    data = open(policy_path, "rb").read()
    if data[:8] != POLICY_MAGIC or data[-8:] != POLICY_MAGIC:
        raise SystemExit(f"{policy_path} is not a bin policy file")
    p = 8 + 8 + 8                      # magic, rho f64, rows u64
    (slen,) = struct.unpack_from("<I", data, p)
    p += 4 + slen + 8                  # source string, source_bytes u64
    (attr_num,) = struct.unpack_from("<I", data, p)
    p += 4
    if attr_num != len(attr_names):
        raise SystemExit(f"{policy_path} has {attr_num} attributes, expected "
                         f"{len(attr_names)}")
    for i in range(attr_num):
        typ, bins = struct.unpack_from("<BI", data, p)
        p += 5
        if typ == 1:                   # kRange: BytesList of bins + 1 bounds
            if attr_names[i] == attr:
                return bins
            (count,) = struct.unpack_from("<I", data, p)
            p += 4
            ends = struct.unpack_from(f"<{count}I", data, p)
            p += 4 * count + (ends[-1] if count else 0)
        else:                          # kEquality: (value, bin) entries
            (count,) = struct.unpack_from("<I", data, p)
            p += 4
            for _ in range(count):
                (vlen,) = struct.unpack_from("<I", data, p)
                p += 4 + vlen + 4
    raise SystemExit(f"{attr} is not a range attribute of {policy_path}")


def load_points(result_dir):
    """{method: [(query selectivity, latency s)]} over the window queries."""
    path = os.path.join(result_dir, QUERIES_CSV)
    if not os.path.exists(path):
        raise SystemExit(f"{path} not found; run summarize_key_correlation.py "
                         "first")
    points = defaultdict(list)
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            if r["family"] != "window":
                continue
            points[r["method"]].append(
                (int(r["records_matched"]) / TOTAL_ROWS,
                 float(r["time_elapsed_ms"]) / 1000.0))
    return points


def binned_medians(points):
    """[(bin centre, median latency s)] over the log-spaced selectivity bins."""
    edges = [SEL_MIN * (SEL_MAX / SEL_MIN) ** (i / SEL_BINS)
             for i in range(SEL_BINS + 1)]
    out = []
    for lo, hi in zip(edges, edges[1:]):
        lat = [y for x, y in points if lo <= x < hi]
        if len(lat) >= MIN_BIN_QUERIES:
            out.append((math.sqrt(lo * hi), statistics.median(lat)))
    return out


def plot(points, global_bin_selectivity, out_path):
    box_w = FIG_W * (RIGHT - LEFT)
    box_h = box_w * PANEL_BOX_ASPECT + 0.01   # a hair of slack, never less
    above, below = LEGEND_H, TICKS_H + XLABEL_H
    fig_h = above + box_h + below
    fig, ax = plt.subplots(figsize=(FIG_W, fig_h))

    for key, label, color, marker in METHODS:
        if key not in points:
            continue
        xs, ys = zip(*binned_medians(points[key]))
        ax.plot(xs, ys, marker=marker, markersize=2.5, linewidth=0.9,
                color=color, label=label)
    # The fraction of rows one global bin holds, named in the caption.
    ax.axvline(global_bin_selectivity, color="#999999", linewidth=0.6,
               linestyle=":")

    ax.set_xscale("log")
    ax.set_yscale("log")
    # Selectivity reads as powers of ten (the paper's sigma notation);
    # latency as plain seconds.
    ax.xaxis.set_major_formatter(mticker.LogFormatterMathtext())
    ax.yaxis.set_major_formatter(mticker.FuncFormatter(lambda v, _: f"{v:g}"))
    ax.xaxis.set_minor_formatter(mticker.NullFormatter())
    ax.yaxis.set_minor_formatter(mticker.NullFormatter())
    ax.set_xlabel(r"Query selectivity $\sigma$")
    ax.set_ylabel("Median query latency (s)")
    ax.set_box_aspect(PANEL_BOX_ASPECT)
    ax.grid(False)

    handles, labels = ax.get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncol=len(labels),
               frameon=False, handlelength=1.8, columnspacing=1.4)
    fig.subplots_adjust(left=LEFT, right=RIGHT, bottom=below / fig_h,
                        top=1 - above / fig_h)
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"Saved: {out_path}")


def main():
    parser = argparse.ArgumentParser(
        description="Plot ULID window-query latency against window width")
    parser.add_argument("result_dir",
                        help=f"read_window result directory ({QUERIES_CSV})")
    parser.add_argument("--bin_policy", required=True,
                        help="Bin policy the BitLSM-Global DB was built with")
    parser.add_argument("--indexed_attrs",
                        default="PULocationID,DOLocationID,tpep_pickup_datetime,"
                                "tpep_dropoff_datetime,fare_amount,"
                                "passenger_count",
                        help="Attribute order the policy was computed with")
    parser.add_argument("-o", "--output-dir", default=None,
                        help="Output directory (default: result_dir)")
    args = parser.parse_args()

    bins = global_bin_count(args.bin_policy, args.indexed_attrs.split(","),
                            START_ATTR)
    print(f"global {START_ATTR} bins: {bins} -> one bin holds "
          f"{1 / bins:.2e} of the rows")

    output_dir = args.output_dir or args.result_dir
    os.makedirs(output_dir, exist_ok=True)
    plot(load_points(args.result_dir), 1.0 / bins,
         os.path.join(output_dir, "key_correlation_latency.pdf"))


if __name__ == "__main__":
    main()
