#!/usr/bin/env python3
"""Reduce a write_perf result directory to one tidy CSV.

ingest.csv already carries one row per (engine, index_layout) run, so this is
a projection rather than an aggregation: it keeps the columns a figure or a
paragraph actually uses, converts bytes to GiB, and adds the ratios against
the layout each cell is meant to be read against.

Two baselines, because the write axis answers two questions:

  * vs_std -- what the index costs. The `std` layout is the same engine with
    no secondary index at all, so this isolates index maintenance from the
    engine's own write path.
  * vs_engine_sk -- what BitLSM costs over conventional secondary indexes,
    the comparison the read side is also read against (bitlsm/sk_bi_v1
    against myrocks/sk_v1: same engine family, same table, index scheme the
    only variable).

Both are omitted when the run being compared against is not in the sweep,
rather than filled with a placeholder.

Usage:
    python3 experiments/myrocks_integration_test/summarize_ingest.py <result_dir>
    python3 experiments/myrocks_integration_test/summarize_ingest.py <result_dir> -o out.csv
"""

import argparse
import csv
import os
import sys

GIB = 1 << 30

# LSM engines share a write path, so an LSM cell's baselines are the MyRocks
# runs -- the sweep carries no bitlsm/std, and a BitLSM build writing an
# unindexed table is MyRocks writing an unindexed table. InnoDB is its own.
SK_BASELINE = {"bitlsm": ("myrocks", "sk_v1"),
               "myrocks": ("myrocks", "sk_v1"),
               "innodb": ("innodb", "sk_v1")}
STD_BASELINE = {"bitlsm": ("myrocks", "std"),
                "myrocks": ("myrocks", "std"),
                "innodb": ("innodb", "std")}


def find_csv(path):
    """Accept either ingest.csv itself or the result directory holding it."""
    if os.path.isfile(path):
        return path
    candidate = os.path.join(path, "ingest.csv")
    if not os.path.isfile(candidate):
        sys.exit(f"No ingest.csv in {path}")
    return candidate


def project(rows):
    out = []
    for r in rows:
        rows_written = int(r["rows"]) if r["rows"] else 0
        db = int(r["db_bytes"]) if r["db_bytes"] else 0
        out.append({
            "workload": r["workload"],
            "engine": r["engine"],
            "index_layout": r["index_layout"],
            "writers": int(r["writers"]) if r["writers"] else None,
            "rows": rows_written,
            "seconds": float(r["seconds"]) if r["seconds"] else None,
            "rows_per_sec": float(r["rows_per_sec"]) if r["rows_per_sec"] else None,
            "cpu_us_per_row": (float(r["server_cpu_us_per_row"])
                               if r.get("server_cpu_us_per_row") else None),
            "db_gib": round(db / GIB, 3) if db else None,
            "bytes_per_row": round(db / rows_written, 1) if rows_written else None,
            "write_amp": float(r["write_amp"]) if r["write_amp"] else None,
        })
    return out


def add_ratios(rows):
    by_key = {(r["workload"], r["engine"], r["index_layout"]): r for r in rows}
    for r in rows:
        w, eng = r["workload"], r["engine"]
        for field, key in (
                ("vs_std", (w,) + STD_BASELINE.get(eng, (eng, "std"))),
                ("vs_engine_sk", (w,) + SK_BASELINE.get(eng, (eng, "sk_v1")))):
            base = by_key.get(key)
            same = base is r
            if not base or same or not base["seconds"] or not r["seconds"]:
                r[field + "_time"] = None
                r[field + "_size"] = None
                continue
            r[field + "_time"] = round(r["seconds"] / base["seconds"], 3)
            r[field + "_size"] = (round(r["db_gib"] / base["db_gib"], 3)
                                  if r["db_gib"] and base["db_gib"] else None)
    rows.sort(key=lambda r: (r["workload"], r["seconds"] or 0))
    return rows


def main():
    ap = argparse.ArgumentParser(
        description="Summarize a write_perf (ingest) sweep to CSV")
    ap.add_argument("result_dir")
    ap.add_argument("-o", "--output", default=None,
                    help="Output CSV (default: <result_dir>/ingest_summary.csv)")
    args = ap.parse_args()

    with open(find_csv(args.result_dir), newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        print("No runs found.")
        return
    summary = add_ratios(project(rows))

    out_path = args.output or os.path.join(
        args.result_dir, "ingest_summary.csv")
    with open(out_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(summary[0].keys()))
        w.writeheader()
        w.writerows(summary)
    print(f"Saved: {out_path}  ({len(summary)} runs)")


if __name__ == "__main__":
    main()
