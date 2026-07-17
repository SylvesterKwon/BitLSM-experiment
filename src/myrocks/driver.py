"""Plan-mode cell driver (harness plan D3/D4).

One cell = (engine, index_layout, query_id, plan, session_vars).
Protocol per cell — always a FRESH session so no state leaks between cells:
  connect -> SET session vars -> EXPLAIN FORMAT=JSON -> optimizer_trace
  -> (optional) ground-truth COUNT -> one result row.

Plan hints for join workloads are injected per bi-carrying table
("title AS t" -> "title AS t FORCE INDEX (bi)"); `auto` runs unhinted.
Execution counters / wallclock (metrics.py) belong to perf-mode (H4).
"""

import json
import os
import re
import time

SESSION_SETUP = [
    'SET SESSION optimizer_trace="enabled=on,one_line=off"',
    "SET SESSION optimizer_trace_max_mem_size=67108864",
]

PLAN_HINTS = {
    "auto": None,
    "force_bi": "FORCE INDEX (bi)",
    "ignore_bi": "IGNORE INDEX (bi)",
}


def inject_table_hint(sql: str, table: str, hint: str) -> str:
    """Append an index hint to every FROM-clause reference of `table`,
    aliased (`X AS y` -> `X AS y HINT`) or bare (`FROM X` -> `FROM X HINT`).
    Raises if nothing matched — a silently un-hinted cell would be
    indistinguishable from `auto`."""
    new, n = re.subn(rf"\b{table}\s+AS\s+(\w+)", rf"{table} AS \1 {hint}",
                     sql, flags=re.IGNORECASE)
    if n == 0:
        new, n = re.subn(rf"\b{table}\b", f"{table} {hint}", sql, count=1,
                         flags=re.IGNORECASE)
    if n == 0:
        raise ValueError(f"hint injection matched nothing for table {table}")
    return new


def _walk_tables(node, out):
    """Collect every `table` entry from an EXPLAIN FORMAT=JSON tree."""
    if isinstance(node, dict):
        if "table" in node and isinstance(node["table"], dict):
            t = node["table"]
            out.append({
                "name": t.get("table_name"),
                "access_type": t.get("access_type"),
                "key": t.get("key"),
                "rows": t.get("rows_examined_per_scan"),
                "filtered": t.get("filtered"),
            })
        for v in node.values():
            _walk_tables(v, out)
    elif isinstance(node, list):
        for v in node:
            _walk_tables(v, out)


def _walk_range_alternatives(node, out):
    """Collect (index, rows, chosen) from every range_scan_alternatives
    block in an optimizer trace — this is where per-index cardinality
    estimates live regardless of which plan was ultimately chosen."""
    if isinstance(node, dict):
        for k, v in node.items():
            if k == "range_scan_alternatives" and isinstance(v, list):
                for alt in v:
                    out.append({
                        "index": alt.get("index"),
                        "rows": alt.get("rows"),
                        "cost": alt.get("cost"),
                        "chosen": alt.get("chosen"),
                    })
            else:
                _walk_range_alternatives(v, out)
    elif isinstance(node, list):
        for v in node:
            _walk_range_alternatives(v, out)


def run_cell(server, database: str, query_id: str, sql: str, plan: str,
             bi_tables=(), session_vars=None, actual_where=None,
             trace_path=None) -> dict:
    """Execute one plan-mode cell on a fresh session; returns a flat dict
    (one CSV row). `actual_where` = {table: where_clause} ground-truth spec;
    counts are the caller's to cache across cells (engine-independent)."""
    hint = PLAN_HINTS[plan]
    q = sql
    if hint:
        if not bi_tables:
            raise ValueError(f"plan={plan} needs bi tables in the layout")
        for t in bi_tables:
            q = inject_table_hint(q, t, hint)

    conn = server.connect(database=database)
    try:
        cur = conn.cursor()
        for stmt in SESSION_SETUP:
            cur.execute(stmt)
        for k, v in (session_vars or {}).items():
            cur.execute(f"SET SESSION {k} = {v}")

        t0 = time.time()
        cur.execute("EXPLAIN FORMAT=JSON " + q)
        explain = json.loads(cur.fetchone()[0])
        explain_ms = (time.time() - t0) * 1000.0

        cur.execute("SELECT trace FROM information_schema.OPTIMIZER_TRACE")
        row = cur.fetchone()
        trace_text = row[0] if row else ""
        if trace_path:
            os.makedirs(os.path.dirname(trace_path), exist_ok=True)
            with open(trace_path, "w") as f:
                f.write(trace_text)

        tables = []
        _walk_tables(explain, tables)
        alts = []
        try:
            _walk_range_alternatives(json.loads(trace_text), alts)
        except (json.JSONDecodeError, TypeError):
            pass  # truncated trace: alts stay empty, row still records sizes

        bi_alt = next((a for a in alts if a["index"] == "bi"), None)
        bi_tab = next((t for t in tables if t["key"] == "bi"), None)

        actuals = {}
        for table, where in (actual_where or {}).items():
            cur.execute(f"SELECT COUNT(*) FROM {table} WHERE {where}")
            actuals[table] = cur.fetchone()[0]

        cost = (explain.get("query_block", {})
                .get("cost_info", {}).get("query_cost"))
        cur.close()
    finally:
        conn.close()

    bi_est = bi_alt["rows"] if bi_alt else None
    actual = next(iter(actuals.values())) if actuals else None
    q_error = None
    if bi_est is not None and actual:
        q_error = round(max(bi_est / actual, actual / bi_est), 4)

    return {
        "query_id": query_id,
        "plan": plan,
        "chosen_plan": json.dumps(
            [[t["name"], t["access_type"], t["key"], t["rows"]]
             for t in tables]),
        "bi_chosen": int(bi_tab is not None),
        "bi_est_rows": bi_est,
        "actual_rows": actual,
        "q_error": q_error,
        "query_cost": cost,
        "explain_ms": round(explain_ms, 1),
        "trace_bytes": len(trace_text),
    }
