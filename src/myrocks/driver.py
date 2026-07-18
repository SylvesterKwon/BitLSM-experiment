"""Plan-mode cell driver (harness plan D3/D4).

One cell = (engine, index_layout, query_id, plan, session_vars).
Protocol per cell — always a FRESH session so no state leaks between cells:
  connect -> SET session vars -> EXPLAIN FORMAT=JSON -> optimizer_trace
  -> (optional) ground-truth COUNT -> one result row.

Plans resolve against the layout's secondary indexes (fail-loud: a plan
that cannot bind to the layout is a config error, never a silent `auto`):
  auto        unhinted — the optimizer's own choice
  force_bi    FORCE INDEX (bi) per bi-carrying table
  ignore_bi   IGNORE INDEX (bi) per bi-carrying table
  fullscan    IGNORE INDEX (<all secondary>) — lower-bound baseline
  index_merge /*+ INDEX_MERGE(t) */ — SK-intersection baseline
Table hints are injected per FROM reference ("title AS t" -> "... t FORCE
INDEX (bi)"); optimizer hints go after the first SELECT.
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

PLANS = ("auto", "force_bi", "ignore_bi", "fullscan", "index_merge")


def resolve_plan(plan: str, secondary_indexes: dict):
    """Map a plan name to concrete hints for a layout, given
    {table: [secondary index names]}. Returns (table_hints, select_hints)
    where table_hints = {table: index-hint string} and select_hints =
    [optimizer-hint bodies]. Raises when the plan cannot bind — a silently
    un-hinted cell would be indistinguishable from `auto`."""
    sec = secondary_indexes or {}
    if plan == "auto":
        return {}, []
    if plan in ("force_bi", "ignore_bi"):
        verb = "FORCE" if plan == "force_bi" else "IGNORE"
        tabs = {t: f"{verb} INDEX (bi)"
                for t, idxs in sec.items() if "bi" in idxs}
        if not tabs:
            raise ValueError(f"plan={plan}: no table carries a bi index "
                             "in this layout")
        return tabs, []
    if plan == "fullscan":
        tabs = {t: "IGNORE INDEX ({})".format(", ".join(idxs))
                for t, idxs in sec.items() if idxs}
        if not tabs:
            raise ValueError("plan=fullscan: layout has no secondary "
                             "indexes to ignore (auto already scans)")
        return tabs, []
    if plan == "index_merge":
        tabs = [t for t, idxs in sec.items()
                if len([i for i in idxs if i != "bi"]) >= 2]
        if not tabs:
            raise ValueError("plan=index_merge: no table has >=2 "
                             "mergeable secondary indexes")
        return {}, [f"INDEX_MERGE({t})" for t in tabs]
    raise ValueError(f"unknown plan: {plan}")


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


def inject_select_hint(sql: str, hints: list) -> str:
    """Insert /*+ ... */ optimizer hints after the first SELECT keyword."""
    body = " ".join(hints)
    new, n = re.subn(r"^\s*SELECT\b",
                     lambda m: m.group(0) + " /*+ " + body + " */",
                     sql, count=1, flags=re.IGNORECASE)
    if n == 0:
        raise ValueError("select-hint injection found no SELECT keyword")
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
             secondary_indexes=None, session_vars=None, actual_where=None,
             trace_path=None) -> dict:
    """Execute one plan-mode cell on a fresh session; returns a flat dict
    (one CSV row). `actual_where` = {table: where_clause} ground-truth spec;
    counts are the caller's to cache across cells (engine-independent)."""
    table_hints, select_hints = resolve_plan(plan, secondary_indexes)
    q = sql
    for t, hint in table_hints.items():
        q = inject_table_hint(q, t, hint)
    if select_hints:
        q = inject_select_hint(q, select_hints)

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

    # plan-sanity contract (H2c): every measurement row names the access
    # path it actually measured; perf-mode CSVs must carry these columns.
    main = tables[0] if tables else {}
    return {
        "query_id": query_id,
        "plan": plan,
        "chosen_access": main.get("access_type"),
        "chosen_key": main.get("key"),
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
