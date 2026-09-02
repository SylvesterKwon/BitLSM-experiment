#!/usr/bin/env python3
"""Flatten a sequential-read result directory into one tidy CSV.

A single sweep drops one per-query log per (selectivity, k, method) cell --
136 files for the r300_q50 set -- and each carries only its own columns, so
the cell it belongs to lives in the filename. This concatenates every log into
one table and promotes that filename into real columns, so the sweep can be
read, grouped or joined without walking the directory.

Only the seven columns honk_player has always written are kept; the
instrumentation columns some builds append (rss, idx_read_mb, ...) are dropped
so the header is the same whichever build produced the logs.

Usage:
    python3 experiments/nyc_taxi_seq_read/summarize.py <result_dir>
    python3 experiments/nyc_taxi_seq_read/summarize.py <result_dir> -o out.csv
"""

import argparse
import csv
import os
import re

FILE_PATTERN = re.compile(
    r"^read_seq_sel([\d.]+)_k(\d+)_r(\d+)_(.+)_read_log\.csv$")

LOG_COLUMNS = [
    "query_id", "query_attr_num", "filter_attrs", "time_elapsed_ms",
    "records_matched", "records_total", "selectivity_actual",
]
COLUMNS = ["method", "selectivity", "k"] + LOG_COLUMNS


def collect(result_dir):
    """Return (rows, repeat_counts) for every read log under result_dir."""
    rows = []
    repeats = set()
    for root, _, files in os.walk(result_dir):
        for fname in sorted(files):
            m = FILE_PATTERN.match(fname)
            if not m:
                continue
            selectivity, k, repeat, method = m.groups()
            repeats.add(repeat)
            with open(os.path.join(root, fname), newline="") as f:
                for r in csv.DictReader(f):
                    row = {
                        "method": method,
                        "selectivity": selectivity,
                        "k": int(k),
                    }
                    row.update({c: r.get(c, "") for c in LOG_COLUMNS})
                    rows.append(row)
    rows.sort(key=lambda r: (float(r["selectivity"]), r["k"], r["method"],
                             int(r["query_id"])))
    return rows, repeats


def main():
    ap = argparse.ArgumentParser(
        description="Flatten nyc_taxi_seq_read per-query logs into one CSV")
    ap.add_argument("result_dir")
    ap.add_argument("-o", "--output", default=None,
                    help="Output CSV "
                         "(default: <result_dir>/nyc_taxi_seq_read_summary.csv)")
    args = ap.parse_args()

    rows, repeats = collect(args.result_dir)
    if not rows:
        print("No read logs found.")
        return

    # The repeat count is not a summary column, so a directory mixing two of
    # them would silently stack unrelated runs in the same cell.
    if len(repeats) > 1:
        print(f"[warn] mixed repeat counts in one directory: "
              f"{', '.join('r' + r for r in sorted(repeats))}")

    out_path = args.output or os.path.join(
        args.result_dir, "nyc_taxi_seq_read_summary.csv")
    with open(out_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=COLUMNS)
        w.writeheader()
        w.writerows(rows)
    cells = len({(r["method"], r["selectivity"], r["k"]) for r in rows})
    print(f"Saved: {out_path}  ({len(rows)} queries across {cells} runs)")


if __name__ == "__main__":
    main()
