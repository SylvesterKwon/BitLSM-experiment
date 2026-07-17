#!/usr/bin/env python3
"""MyRocks integration test — plan-mode sweep (SSB-flat).

Tests the BitLSM x MyRocks integration end-to-end at the SQL level: index
selection (auto vs hinted plans), estimator quality (q-error vs cached
ground truth), and per-engine plan choices over the same data.

For each (engine, index_layout) cell: boot the identity-cached datadir,
then for each query x plan run one plan-mode cell (fresh session; EXPLAIN
JSON + optimizer_trace) and append a CSV row. Ground-truth COUNTs are
engine-independent (all engines load the byte-identical flat file) and are
cached once per (workload, query) under the data directory.

Usage:
    python3 experiments/myrocks_integration_test/run.py exp_set/<params>.json [options]
"""

import csv
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "src"))

from run_common import (  # noqa: E402
    add_common_args, make_result_dir, maybe_run_as_daemon,
    setup_logging, teardown_logging,
)
from myrocks import driver as drv  # noqa: E402
from myrocks.loader import datadir_for, ensure_loaded, is_loaded, server_for  # noqa: E402
from myrocks.workloads.ssb_flat import SsbFlatWorkload  # noqa: E402

CSV_FIELDS = [
    "ts", "workload", "engine", "index_layout", "query_id", "plan",
    "bi_chosen", "bi_est_rows", "actual_rows", "q_error", "query_cost",
    "explain_ms", "trace_bytes", "chosen_plan",
    "lsm_state", "server_args_hash", "mysql_commit", "bitlsm_commit",
]


def make_workload(config):
    if config["workload"] == "ssbflat":
        return SsbFlatWorkload(sf=config.get("sf", 1))
    raise ValueError(f"unknown workload: {config['workload']}")


def extract_where(sql: str):
    """Single-table flat queries only: text between WHERE and GROUP/ORDER."""
    m = re.search(r"\bWHERE\b(.*?)(\bGROUP BY\b|\bORDER BY\b|;|$)",
                  sql, re.S | re.I)
    return m.group(1).strip() if m else None


class GroundTruth:
    """(query_id -> actual matching rows), persisted next to the data so
    every engine and estimator iteration scores against the same truth."""

    def __init__(self, workload):
        self.path = os.path.join(
            os.path.dirname(workload.data_file("lineorder_flat")),
            f"ground_truth.{workload.name}.json")
        self.cache = {}
        if os.path.exists(self.path):
            with open(self.path) as f:
                self.cache = json.load(f)

    def get(self, qid, sql, conn):
        if qid not in self.cache:
            where = extract_where(sql)
            if where is None:
                return None
            cur = conn.cursor()
            cur.execute(
                f"SELECT COUNT(*) FROM lineorder_flat WHERE {where}")
            self.cache[qid] = cur.fetchone()[0]
            cur.close()
            with open(self.path, "w") as f:
                json.dump(self.cache, f, indent=2, sort_keys=True)
        return self.cache[qid]


def run(config_path, dry_run, start_from):
    with open(config_path) as f:
        config = json.load(f)

    exp_set = os.path.splitext(os.path.basename(config_path))[0]
    exp_name = os.path.basename(os.path.dirname(os.path.abspath(__file__)))
    exp_label = f"{exp_name}_{exp_set}"
    workload = make_workload(config)
    build_kind = config.get("build_kind", "debug")
    queries = workload.queries()
    if config.get("queries", "all") != "all":
        queries = {q: queries[q] for q in config["queries"]}

    cells = config["cells"]
    total = sum(len(queries) * len(c["plans"]) for c in cells)
    out_dir = make_result_dir(exp_label)
    log_file, log_path = setup_logging(exp_label)
    try:
        print(f"config : {config_path}")
        print(f"label  : {exp_label}")
        print(f"cells  : {len(cells)} x {len(queries)} queries -> {total} rows")
        print(f"output : {out_dir}")
        if dry_run:
            for c in cells:
                identity = workload.identity(c["engine"], c["index_layout"])
                state = "cached" if is_loaded(identity) else "NEEDS LOAD"
                print(f"  [{c['engine']}/{c['index_layout']}] {state} "
                      f"plans={c['plans']}")
            return

        os.makedirs(out_dir, exist_ok=True)
        csv_path = os.path.join(out_dir, "plan_mode.csv")
        truth = GroundTruth(workload)
        idx = 0
        with open(csv_path, "w", newline="") as cf:
            writer = csv.DictWriter(cf, fieldnames=CSV_FIELDS)
            writer.writeheader()

            for cell in cells:
                engine, layout = cell["engine"], cell["index_layout"]
                identity = workload.identity(engine, layout)
                ensure_loaded(workload, engine, layout, build_kind)
                with open(os.path.join(datadir_for(identity),
                                       "LOADED.json")) as f:
                    marker = json.load(f)
                srv = server_for(workload, engine, layout, build_kind)
                bi_tabs = workload.bi_tables(layout)
                with srv:
                    binfo = srv.build_info()
                    truth_conn = srv.connect(database=workload.name)
                    for qid, sql in queries.items():
                        actual = truth.get(qid, sql, truth_conn)
                        for plan in cell["plans"]:
                            idx += 1
                            if idx < start_from:
                                continue
                            trace_path = os.path.join(
                                out_dir, "traces",
                                f"{engine}-{layout}-{qid}-{plan}.json")
                            row = drv.run_cell(
                                srv, workload.name, qid, sql, plan,
                                bi_tables=bi_tabs,
                                session_vars=cell.get("session_vars"),
                                trace_path=trace_path)
                            row["actual_rows"] = actual
                            if row["bi_est_rows"] and actual:
                                row["q_error"] = round(max(
                                    row["bi_est_rows"] / actual,
                                    actual / row["bi_est_rows"]), 4)
                            row.update({
                                "ts": time.strftime("%H:%M:%S"),
                                "workload": workload.name,
                                "engine": engine,
                                "index_layout": layout,
                                "lsm_state": marker.get("lsm_state"),
                                "server_args_hash": srv.args_hash(),
                                "mysql_commit": binfo["mysql_commit"][:12],
                                "bitlsm_commit": binfo["bitlsm_commit"][:12],
                            })
                            writer.writerow(row)
                            cf.flush()
                            print(f"[{idx}/{total}] {engine}/{layout} "
                                  f"{qid} {plan}: bi_chosen={row['bi_chosen']} "
                                  f"est={row['bi_est_rows']} act={actual} "
                                  f"qerr={row['q_error']}")
                    truth_conn.close()
        print(f"done -> {csv_path}")
    finally:
        teardown_logging(log_file, log_path)


def main():
    import argparse
    parser = argparse.ArgumentParser(
        description="MyRocks integration test sweep")
    add_common_args(parser)
    args = parser.parse_args()
    maybe_run_as_daemon(args)
    run(args.config, dry_run=args.dry_run, start_from=args.start_from)


if __name__ == "__main__":
    main()
