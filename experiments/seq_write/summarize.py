#!/usr/bin/env python3
"""Reduce a sequential-write master CSV to one row per run.

run.py appends a checkpoint row every 1M records, so a sweep's master CSV is
mostly progress rows: the run's own result is the last row of each block, the
only one carrying db_size_bytes (the DB is measured once, after the run).
This keeps those rows and drops the rest, leaving one row per (method, a).

The attribute count is emitted as 'a', matching the schema names the sweep
runs over (default_a16); master CSVs old enough to carry a schema stem instead
of attr_count are read through the same column.

Usage:
    python3 experiments/seq_write/summarize.py <result_dir_or_master_csv>
    python3 experiments/seq_write/summarize.py <result_dir> -o out.csv
"""

import argparse
import csv
import os
import re
import sys

COLUMNS = ["method", "a", "time_elapsed_ms", "records_written",
           "db_size_bytes", "drain_ms"]

SCHEMA_ATTRS = re.compile(r"_a(\d+)")


def find_master_csv(path):
    """Accept either the master CSV itself or the result directory holding it."""
    if os.path.isfile(path):
        return path
    candidates = []
    for fname in sorted(os.listdir(path)):
        if not fname.endswith(".csv") or fname.endswith("_summary.csv"):
            continue
        full = os.path.join(path, fname)
        with open(full, newline="") as f:
            header = next(csv.reader(f), [])
        if header[:1] == ["method"] and "db_size_bytes" in header:
            candidates.append(full)
    if not candidates:
        sys.exit(f"No master CSV found in {path}")
    if len(candidates) > 1:
        sys.exit("Multiple master CSVs found; pass one explicitly:\n  "
                 + "\n  ".join(candidates))
    return candidates[0]


def attr_value(row):
    if row.get("attr_count"):
        return row["attr_count"]
    m = SCHEMA_ATTRS.search(row.get("schema", "") or "")
    return m.group(1) if m else ""


def collect(master_csv):
    """Keep each run's final row, in the order the sweep produced them."""
    rows = []
    with open(master_csv, newline="") as f:
        pending = None
        for r in csv.DictReader(f):
            out = {
                "method": r.get("method", ""),
                "a": attr_value(r),
                "time_elapsed_ms": r.get("time_elapsed_ms", ""),
                "records_written": r.get("records_written", ""),
                "db_size_bytes": r.get("db_size_bytes", "") or "",
                "drain_ms": r.get("drain_ms", "") or "",
            }
            if out["db_size_bytes"]:
                rows.append(out)
                pending = None
            else:
                pending = out
        # A run killed mid-flight never got its db_size_bytes row; keep its
        # last checkpoint so the sweep's tail is not silently missing.
        if pending is not None:
            print("[warn] last run has no final row — "
                  "keeping its last checkpoint, db_size_bytes empty")
            rows.append(pending)
    return rows


def main():
    ap = argparse.ArgumentParser(
        description="Reduce a seq_write master CSV to one row per run")
    ap.add_argument("target", help="Result directory or master CSV path")
    ap.add_argument("-o", "--output", default=None,
                    help="Output CSV (default: "
                         "<result_dir>/seq_write_summary.csv)")
    args = ap.parse_args()

    master_csv = find_master_csv(args.target)
    rows = collect(master_csv)
    if not rows:
        print(f"No runs found in {master_csv}")
        return

    out_path = args.output or os.path.join(
        os.path.dirname(master_csv), "seq_write_summary.csv")
    with open(out_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=COLUMNS)
        w.writeheader()
        w.writerows(rows)
    print(f"Saved: {out_path}  ({len(rows)} runs from {master_csv})")


if __name__ == "__main__":
    main()
