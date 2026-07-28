"""Perf-mode cell driver (harness plan D3 perf-mode, D4, D7).

One perf cell = (engine, index_layout, query_id, plan) measured on a
freshly booted server: execution 0 is the cold run (the runner restarts
the server per cell, which empties the engine cache — with D7 direct I/O
that is what makes it cold), followed by K warm repetitions on the same
connection. Counters are GLOBAL-status snapshot deltas around each
execution (valid because the server is quiescent — nothing else runs).
Wallclock is client-side around execute+fetchall.

The plan-sanity contract (H2c) carries over: every row records
chosen_access/chosen_key, taken from EXPLAIN run AFTER the timed
executions so the cold run stays pristine. Result rows are fingerprinted
(md5 over sorted row reprs — order-independent) so cross-engine result
equality is checkable by the runner: this extends the H2 COUNT gate to
full result sets. A fingerprint change BETWEEN repetitions of the same
cell is a hard error.
"""

import hashlib
import json
import os
import time

from .driver import (_walk_tables, inject_select_hint, inject_table_hint,
                     resolve_plan)

# First-class CSV counters (cold-run deltas; engine-inapplicable ones stay
# empty). Everything else lands in the per-cell sidecar JSON.
KEY_COUNTERS = [
    "handler_read_key", "handler_read_next", "handler_read_rnd_next",
    "rocksdb_rows_read", "rocksdb_number_multiget_keys_read",
    "rocksdb_block_cache_miss", "rocksdb_bytes_read",
    "innodb_buffer_pool_reads", "innodb_data_read",
]

_STATUS_PATTERNS = ("Handler_%", "Rocksdb_%", "Innodb_%")


def _status_snapshot(cur) -> dict:
    """{lowercased counter: int} for the numeric Handler_*/engine status."""
    snap = {}
    for like in _STATUS_PATTERNS:
        cur.execute(f"SHOW GLOBAL STATUS LIKE '{like}'")
        for name, value in cur.fetchall():
            try:
                snap[name.lower()] = int(value)
            except (TypeError, ValueError):
                pass
    return snap


def _norm_cell(v):
    """Floats to 12 significant digits before fingerprinting: AVG/SUM over
    DOUBLE is summation-order dependent, so bit-exact digests would falsely
    differ across access paths (registry: fingerprint-float-12g). Exact
    types (int/Decimal/str/bytes/None) pass through untouched."""
    return f"{v:.12g}" if isinstance(v, float) else v


def _fingerprint(rows) -> str:
    h = hashlib.md5()
    for r in sorted(repr(tuple(_norm_cell(c) for c in r)) for r in rows):
        h.update(r.encode())
    return h.hexdigest()


def run_perf_cell(server, database: str, query_id: str, sql: str, plan: str,
                  secondary_indexes=None, session_vars=None, warm_reps=5,
                  sidecar_path=None) -> dict:
    """Measure one perf cell; returns a flat dict (one CSV row). The caller
    owns the cold protocol (fresh server per cell); execution 0 here is
    that cold run."""
    table_hints, select_hints = resolve_plan(plan, secondary_indexes)
    q = sql
    for t, hint in table_hints.items():
        q = inject_table_hint(q, t, hint)
    if select_hints:
        q = inject_select_hint(q, select_hints)

    conn = server.connect(database=database)
    try:
        cur = conn.cursor()
        for k, v in (session_vars or {}).items():
            cur.execute(f"SET SESSION {k} = {v}")

        execs = []
        fingerprint = None
        for _ in range(1 + warm_reps):
            before = _status_snapshot(cur)
            t0 = time.perf_counter()
            cur.execute(q)
            rows = cur.fetchall()
            ms = (time.perf_counter() - t0) * 1000.0
            after = _status_snapshot(cur)
            delta = {k: after[k] - before.get(k, 0)
                     for k in after if after[k] != before.get(k, 0)}
            fp = _fingerprint(rows)
            if fingerprint is None:
                fingerprint = fp
            elif fp != fingerprint:
                raise RuntimeError(
                    f"{query_id}/{plan}: result changed between repetitions "
                    f"({fingerprint} -> {fp}) on read-only data")
            execs.append({"ms": round(ms, 1), "rows": len(rows),
                          "delta": delta})

        # plan sanity AFTER the timed runs (cold run stays pristine)
        cur.execute("EXPLAIN FORMAT=JSON " + q)
        explain = json.loads(cur.fetchone()[0])
        tables = []
        _walk_tables(explain, tables)
        main = tables[0] if tables else {}
        cur.close()
    finally:
        conn.close()

    if sidecar_path:
        os.makedirs(os.path.dirname(sidecar_path), exist_ok=True)
        with open(sidecar_path, "w") as f:
            json.dump({"query_id": query_id, "plan": plan, "sql": q,
                       "executions": execs}, f, indent=2)

    cold = execs[0]
    warm = sorted(e["ms"] for e in execs[1:])
    row = {
        "query_id": query_id,
        "plan": plan,
        "chosen_access": main.get("access_type"),
        "chosen_key": main.get("key"),
        "cold_ms": cold["ms"],
        "warm_ms_median": warm[len(warm) // 2] if warm else None,
        "warm_ms_min": warm[0] if warm else None,
        "warm_ms_max": warm[-1] if warm else None,
        "warm_reps": len(warm),
        "rows_returned": cold["rows"],
        "result_fingerprint": fingerprint,
    }
    for c in KEY_COUNTERS:
        row[c] = cold["delta"].get(c)
    return row
