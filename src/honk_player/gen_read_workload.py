#!/usr/bin/env python3
"""Generate read workloads calibrated to a specific write workload.

Query predicates are only meaningful against the dataset they were sized for:
a query set built for the 2024-2025 dump returns almost nothing against a
2025-only DB. This tool samples the write TSV, then searches for filter
combinations whose measured selectivity on that sample lands near the target.

Emitted TSV lines match honk_player's reader:

    r\t{"filters": [...], "most_selective_attr": "..."}

Also emits a prune-all set: filters that fall outside the data range entirely,
so every SST is pruned and no data block is read. Those queries isolate index
residency -- any block-cache miss they cause is an index blob being re-read.

Usage:
    python3 src/honk_player/gen_read_workload.py \
        --workload /home/dohyun/Workloads/write_seq_2025_all.tsv \
        --out workloads/read_seq_2025
"""

import argparse
import json
import os
import pickle
import random
import re
import sys

# Indexed attributes, split by the role honk_player's schema gives them
# (src/honk_player/taxi_schema.h): equality on unordered, range on ordered.
EQ_ATTRS = ["PULocationID", "DOLocationID"]
RANGE_ATTRS = [
    "tpep_pickup_datetime",
    "tpep_dropoff_datetime",
    "fare_amount",
    "passenger_count",
]
ALL_ATTRS = EQ_ATTRS + RANGE_ATTRS

FIELD_RE = {
    a: re.compile(r'"%s":\s*(-?[0-9.]+)' % a) for a in ALL_ATTRS
}


def sample_rows(path: str, every: int, cache: str):
    """Every `every`-th record of the write TSV, as dicts of the indexed attrs."""
    if cache and os.path.exists(cache):
        with open(cache, "rb") as f:
            rows = pickle.load(f)
        print(f"sample: {len(rows)} rows (cached {cache})")
        return rows

    rows = []
    with open(path, "r") as f:
        for i, line in enumerate(f):
            if i % every:
                continue
            if not line.startswith("w\t") and not line.startswith("u\t"):
                continue
            row = {}
            for attr, rx in FIELD_RE.items():
                m = rx.search(line)
                if m:
                    row[attr] = float(m.group(1))
            if len(row) == len(ALL_ATTRS):
                rows.append(row)
    print(f"sample: {len(rows)} rows from {path} (every {every}th)")
    if cache:
        with open(cache, "wb") as f:
            pickle.dump(rows, f)
    return rows


def selectivity(rows, preds) -> float:
    """Fraction of sampled rows satisfying every predicate."""
    hits = 0
    for r in rows:
        for p in preds:
            v = r[p["attr"]]
            if p["op"] == "eq":
                if v != p["value"]:
                    break
            elif not (p["lo"] <= v <= p["hi"]):
                break
        else:
            hits += 1
    return hits / len(rows) if rows else 0.0


def attr_stats(rows):
    """Per-attribute value list (sorted) for range sizing, and eq value counts."""
    stats = {}
    for a in RANGE_ATTRS:
        stats[a] = sorted(r[a] for r in rows)
    counts = {}
    for a in EQ_ATTRS:
        c = {}
        for r in rows:
            c[r[a]] = c.get(r[a], 0) + 1
        counts[a] = c
    return stats, counts


def make_range(sorted_vals, frac, rng):
    """A [lo, hi] window covering `frac` of the sampled values, placed randomly."""
    n = len(sorted_vals)
    width = max(1, int(n * frac))
    start = rng.randrange(0, max(1, n - width))
    return sorted_vals[start], sorted_vals[min(n - 1, start + width)]


def build_query(rows, stats, counts, k, target, rng, tries=60):
    """Search for a k-attribute query whose sample selectivity is near target."""
    for _ in range(tries):
        preds = []
        # One equality anchor (the most selective attribute), then ranges.
        eq_attr = rng.choice(EQ_ATTRS)
        eq_value = rng.choice(list(counts[eq_attr].keys()))
        eq_sel = counts[eq_attr][eq_value] / len(rows)
        preds.append({"attr": eq_attr, "op": "eq", "value": int(eq_value)})

        remaining = k - 1
        if remaining:
            # Split the leftover selectivity budget evenly across the ranges,
            # then let the measured value decide whether to keep the query.
            per = (target / eq_sel) ** (1.0 / remaining) if eq_sel > 0 else 1.0
            per = min(1.0, max(1e-4, per))
            for a in rng.sample(RANGE_ATTRS, remaining):
                lo, hi = make_range(stats[a], per, rng)
                preds.append({"attr": a, "op": "range", "lo": lo, "hi": hi})

        sel = selectivity(rows, preds)
        if target / 3 <= sel <= target * 3:
            return preds, sel
    return None, 0.0


def emit(path, queries):
    with open(path, "w") as f:
        for preds, _ in queries:
            f.write("r\t" + json.dumps(
                {"filters": preds, "most_selective_attr": preds[0]["attr"]}
            ) + "\n")
    print(f"wrote {path} ({len(queries)} queries)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--workload", required=True, help="write TSV to calibrate against")
    ap.add_argument("--out", required=True, help="output directory")
    ap.add_argument("--every", type=int, default=100, help="sample every Nth record")
    ap.add_argument("--cache", default="/scratch/read_workload_sample.pkl")
    ap.add_argument("--k", default="2,3,4", help="comma-separated attribute counts")
    ap.add_argument("--sel", default="0.001,0.01", help="comma-separated targets")
    ap.add_argument("--count", type=int, default=50, help="queries per (k, sel)")
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    rng = random.Random(args.seed)
    rows = sample_rows(args.workload, args.every, args.cache)
    if not rows:
        sys.exit("no rows sampled -- check the workload path/format")
    stats, counts = attr_stats(rows)
    os.makedirs(args.out, exist_ok=True)

    for target in [float(s) for s in args.sel.split(",")]:
        for k in [int(s) for s in args.k.split(",")]:
            queries = []
            while len(queries) < args.count:
                preds, sel = build_query(rows, stats, counts, k, target, rng)
                if preds is None:
                    print(f"  sel={target} k={k}: giving up at {len(queries)}")
                    break
                queries.append((preds, sel))
            if queries:
                mean = sum(s for _, s in queries) / len(queries)
                print(f"  sel={target} k={k}: mean sampled selectivity {mean:.5f}")
                emit(os.path.join(
                    args.out, f"read_sel{target}_k{k}_r{len(queries)}.tsv"), queries)

    # Prune-all probe: a pickup-datetime window entirely below the data range,
    # so every SST is excluded and the only blocks touched are index blobs.
    lo_bound = stats["tpep_pickup_datetime"][0]
    prune = [([{
        "attr": "PULocationID", "op": "eq", "value": int(rng.choice(list(counts["PULocationID"].keys()))),
    }, {
        "attr": "tpep_pickup_datetime", "op": "range",
        "lo": lo_bound - 10_000_000, "hi": lo_bound - 9_000_000,
    }], 0.0) for _ in range(args.count)]
    emit(os.path.join(args.out, f"read_pruneall_k2_r{len(prune)}.tsv"), prune)


if __name__ == "__main__":
    main()
