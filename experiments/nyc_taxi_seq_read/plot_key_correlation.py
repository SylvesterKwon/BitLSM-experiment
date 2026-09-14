#!/usr/bin/env python3
"""Plot the ULID key-correlation subsection: latency against window width.

One single-column panel. Every query of the interval_window workloads
(pickup >= x AND dropoff < y) is placed at its own window width y - x, the
queries are cut into log-spaced width bins, and each bin's MEDIAN latency is
drawn per method. Width is the one thing that varies across the 900 queries --
they share one template and one DB -- so the binned medians form a curve, and
the curve is the mechanism: BitLSM-Global sits on a floor set by the width of
a global bin (it fetches the whole bin however narrow the window), BitLSM
tracks the window because its per-SST bins are minutes wide, and the two meet
where the window outgrows a global bin. Per-Block Filters lie on BitLSM
throughout: block zone maps prune fully once the predicate is on the
key-correlated attribute.

The dotted vertical line is the median width of the global pickup bins, read
from the bin policy the BitLSM-Global DB was built with. The line carries no
label in the figure; the caption names it.

Inputs: the read_window result directory (key_correlation_queries.csv from
summarize_key_correlation.py), the workload TSVs the bounds come from, and
the bin policy file.

Usage:
    python3 experiments/nyc_taxi_seq_read/plot_key_correlation.py <window_result_dir> \
        --bin_policy /scratch/honk/bin_policy/write_seq_2024-2025_all_ulid_rho0.001.bin \
        [--workload_dir workloads/read_seq] [-o <output_dir>]
"""

import argparse
import csv
import glob
import json
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

# Height/width of the plot box, measured from nyc_taxi_seq_read's 4x4 grid
# (1.294 x 0.909 in). A lone panel does not reach it on its own, so it is
# pinned, and the figure height is derived from the box plus the furniture it
# carries rather than left to tight_layout.
PANEL_BOX_ASPECT = 0.702
FIG_W = 3.333               # single text column, inches
LEFT, RIGHT = 0.135, 0.99   # room for the y label and "0.01" tick labels
LEGEND_H = 0.20             # legend strip above the box
TICKS_H = 0.13              # x tick labels under the box
XLABEL_H = 0.12             # x label under the ticks

# (method key as in the result file names, legend label, color, marker).
# Per-Block Filters and BitLSM keep nyc_taxi_seq_read/plot.py's colors;
# BitLSM-Global's orange is its own convention (CLAUDE.md figure style).
METHODS = [
    ("embedded_bloom_bits10", "Per-Block Filters", "#1FA8A0", "P"),
    ("bitlsm-global_rho0.001", "BitLSM-Global", "#E69F00", "D"),
    ("bitlsm_rho0.001", "BitLSM", "#9B1B1B", "o"),
]
START_ATTR = "tpep_pickup_datetime"   # the window's lower bound, and the key
END_ATTR = "tpep_dropoff_datetime"

# Width bins: 12 log-spaced bins from 0.3 h to 300 h cover every window the
# three selectivity bands produce (0.3 h to 240 h). A bin holding fewer
# queries than MIN_BIN_QUERIES is not drawn -- its median would be one or two
# queries' noise.
WIDTH_BINS = 12
WIDTH_MIN_H, WIDTH_MAX_H = 0.3, 300.0
MIN_BIN_QUERIES = 10

QUERIES_CSV = "key_correlation_queries.csv"
POLICY_MAGIC = b"GBINPOL1"


def okey_to_f64(b):
    """Undo bit_lsm::F64ToOkey (bit_lsm_encoding.h) on an 8-byte boundary."""
    okey = int.from_bytes(b, "big")
    if okey & (1 << 63):
        u = okey ^ (1 << 63)
    else:
        u = (~okey) & ((1 << 64) - 1)
    return struct.unpack("<d", struct.pack("<Q", u))[0]


def global_bin_widths_h(policy_path, attr_names, attr):
    """Widths in hours of `attr`'s bins in a bin policy file (bin_policy.cpp
    layout: header, then per attribute [type u8][bins u32] and a BytesList
    of boundaries for range attributes or (value, bin) entries otherwise)."""
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
            (count,) = struct.unpack_from("<I", data, p)
            p += 4
            ends = struct.unpack_from(f"<{count}I", data, p)
            p += 4 * count
            arena = data[p:p + ends[-1]]
            p += ends[-1]
            if attr_names[i] == attr:
                starts = (0,) + ends[:-1]
                bounds = [okey_to_f64(arena[s:e]) for s, e in zip(starts, ends)]
                return [(hi - lo) / 3600 for lo, hi in zip(bounds, bounds[1:])]
        else:                          # kEquality: (value, bin) entries
            (count,) = struct.unpack_from("<I", data, p)
            p += 4
            for _ in range(count):
                (vlen,) = struct.unpack_from("<I", data, p)
                p += 4 + vlen + 4
    raise SystemExit(f"{attr} is not a range attribute of {policy_path}")


def window_widths_h(workload_dir, selectivity):
    """{query_id: window width in hours} of one band's workload TSV."""
    paths = glob.glob(os.path.join(workload_dir,
                                   f"read_window_sel{selectivity}_r*.tsv"))
    if len(paths) != 1:
        raise SystemExit(f"expected one read_window_sel{selectivity}_r*.tsv "
                         f"in {workload_dir}, found {len(paths)}")
    widths = {}
    with open(paths[0]) as f:
        for qid, line in enumerate(f):
            filters = json.loads(line.split("\t", 1)[1])["filters"]
            lo = next(fl["lo"] for fl in filters if fl["attr"] == START_ATTR)
            hi = next(fl["hi"] for fl in filters if fl["attr"] == END_ATTR)
            widths[qid] = (hi - lo) / 3600
    return widths


def load_points(result_dir, workload_dir):
    """{method: [(window width h, latency s)]} over the window queries."""
    path = os.path.join(result_dir, QUERIES_CSV)
    if not os.path.exists(path):
        raise SystemExit(f"{path} not found; run summarize_key_correlation.py "
                         "first")
    widths = {}
    points = defaultdict(list)
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            if r["family"] != "window":
                continue
            if r["selectivity"] not in widths:
                widths[r["selectivity"]] = window_widths_h(workload_dir,
                                                           r["selectivity"])
            points[r["method"]].append(
                (widths[r["selectivity"]][int(r["query_id"])],
                 float(r["time_elapsed_ms"]) / 1000.0))
    return points


def binned_medians(points):
    """[(bin centre h, median latency s)] over the log-spaced width bins."""
    edges = [WIDTH_MIN_H * (WIDTH_MAX_H / WIDTH_MIN_H) ** (i / WIDTH_BINS)
             for i in range(WIDTH_BINS + 1)]
    out = []
    for lo, hi in zip(edges, edges[1:]):
        lat = [y for x, y in points if lo <= x < hi]
        if len(lat) >= MIN_BIN_QUERIES:
            out.append((math.sqrt(lo * hi), statistics.median(lat)))
    return out


def plot(points, global_bin_h, out_path):
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
    # The global bin width, named in the caption.
    ax.axvline(global_bin_h, color="#999999", linewidth=0.6, linestyle=":")

    ax.set_xscale("log")
    ax.set_yscale("log")
    for axis in (ax.xaxis, ax.yaxis):
        axis.set_major_formatter(mticker.FuncFormatter(lambda v, _: f"{v:g}"))
        axis.set_minor_formatter(mticker.NullFormatter())
    ax.set_xlabel("Window width (h)")
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
    parser.add_argument("--workload_dir",
                        default=os.path.join(os.path.dirname(__file__), "..",
                                             "..", "workloads", "read_seq"),
                        help="Where the read_window_*.tsv workloads live")
    parser.add_argument("-o", "--output-dir", default=None,
                        help="Output directory (default: result_dir)")
    args = parser.parse_args()

    widths = global_bin_widths_h(args.bin_policy, args.indexed_attrs.split(","),
                                 START_ATTR)
    global_bin_h = statistics.median(widths)
    print(f"global {START_ATTR} bin width: median {global_bin_h:.2f} h "
          f"over {len(widths)} bins")

    output_dir = args.output_dir or args.result_dir
    os.makedirs(output_dir, exist_ok=True)
    plot(load_points(args.result_dir, args.workload_dir), global_bin_h,
         os.path.join(output_dir, "key_correlation_latency.pdf"))


if __name__ == "__main__":
    main()
