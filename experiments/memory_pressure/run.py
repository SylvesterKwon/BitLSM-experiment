#!/usr/bin/env python3
"""
NYC Taxi READ experiment — block-cache budget sweep.

Same read replay as nyc_taxi_seq_read (honk_player, pre-built DBs), but adds a
memory-budget axis: each run is repeated at every block_cache size listed under
"block_cache_mb" in the param set. The budget is applied to EVERY method through
EXP_BLOCK_CACHE_MB (a single process-wide LRU cache shared by all column
families; see src/bindings/rocksdb_common_option.h). A budget of 0 means "no
override" — RocksDB defaults, i.e. the standard config.

This measures how each index (BitLSM SABI, Embedded Postings, embedded BF+ZM, si-*
native CFs, no-index) degrades as the total resident memory shrinks. It is a
READ experiment; the DB must already exist (built by nyc_taxi_seq_write).

Interpretation:
    Time (time_elapsed_ms in the read CSV) is the headline metric — it is
    the only column that is directly comparable across every method. Embedded Postings'
    disk_read_mb is inflated relative to its useful payload: on-demand
    interior-leaf reads are pinned as one coarse span per (attr, SST, query
    range), and that span interleaves the leaves' values|perm bytes between
    their postings (records are values|perm|postings back to back), so the
    coarse read carries dead bytes that were never requested. Byte columns
    (disk_read_mb, rchar_mb) are therefore not cross-method comparable; only
    time is. Per-table metadata directories (the small per-table index
    roots BitLSM/Embedded Postings/embedded each keep resident to locate their on-demand
    extents) live in the process heap outside EXP_BLOCK_CACHE_MB for ALL
    three UDI methods alike — this is symmetric across bitlsm/embedded-postings/embedded,
    not a handicap specific to one of them. ALWAYS verify build/CMakeCache.txt
    says CMAKE_BUILD_TYPE=Release before measuring — a Debug build left in
    place silently inflates every number by roughly 3x with no other symptom.

Usage:
    python3 experiments/memory_pressure/run.py \
        experiments/memory_pressure/exp_set/<params>.json \
        --pk_mode uuid [options]
"""

import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "src"))

from run_common import (
    add_common_args,
    cartesian_combinations,
    cooldown_sleep,
    fmt,
    make_result_dir,
    maybe_run_as_daemon,
    parse_method_filter,
    PROJECT_ROOT,
    reset_hardware,
    resolve_workload_paths,
    run_process,
    setup_logging,
    teardown_logging,
)

EXP_DIR = os.path.dirname(os.path.abspath(__file__))
BINARY = os.path.join(PROJECT_ROOT, "build", "bin", "honk_player")

DB_PARAMS = ["rho", "bloom_bits"]


def encode_method_params(params: dict) -> str:
    db_params = {k: v for k, v in params.items() if k in DB_PARAMS}
    if not db_params:
        return "default"
    return "_".join(f"{k}{fmt(v)}" for k, v in db_params.items())


def workload_stem(path: str) -> str:
    return os.path.splitext(os.path.basename(path))[0]


def build_command(method_name: str, workload: str, db_path: str,
                  output_dir: str, combo: dict, common_params: dict,
                  query_limit: int = 0) -> list:
    cmd = [BINARY,
           "--binding", method_name,
           "--workload", workload,
           "--db_path", db_path,
           "--output_dir", output_dir]
    for key, val in common_params.items():
        cmd += [f"--{key}", fmt(val)]
    for key, val in combo.items():
        cmd += [f"--{key}", fmt(val)]
    if query_limit > 0:
        cmd += ["--query_limit", str(query_limit)]
    return cmd


def run(config_path: str, dry_run: bool, method_filter: list,
        cooldown: int, hw_reset: bool, pk_mode: str, warmup: bool = False,
        start_from: int = 1):
    with open(config_path) as f:
        config = json.load(f)

    exp_set_name  = os.path.splitext(os.path.basename(config_path))[0]
    exp_name      = os.path.basename(EXP_DIR)
    exp_label     = f"{exp_name}_{exp_set_name}"
    db_path_base  = config["db_path_base"]
    workloads     = resolve_workload_paths(config["workload"])
    methods       = config["methods"]
    common_params = config.get("common_params", {})
    # Budget axis. 0 = no override (RocksDB default / standard config).
    budgets       = config.get("block_cache_mb", [0])
    output_dir    = make_result_dir(exp_label)

    log_file, log_path = setup_logging(exp_label)

    try:
        if method_filter:
            methods = [m for m in methods if m["name"] in method_filter]
            if not methods:
                sys.exit(f"No methods matched: {method_filter}")

        # A method may pin itself to a subset of the budget axis via its own
        # "block_cache_mb" allowlist (e.g. resident-mode baselines run only at
        # 0 = unbounded; sweeping a resident index through tight budgets
        # measures the eviction/re-parse pathology, not a baseline).
        def method_budgets(m):
            allow = m.get("block_cache_mb")
            return [b for b in budgets if allow is None or b in allow]

        total_runs = len(workloads) * sum(
            len(method_budgets(m)) * len(cartesian_combinations(m.get("params", {})))
            for m in methods
        )
        print(f"config   : {config_path}")
        print(f"label    : {exp_label}")
        combo_total = sum(
            len(cartesian_combinations(m.get("params", {}))) for m in methods)
        print(f"axes     : {len(workloads)} workload(s) x {len(budgets)} budget(s) "
              f"x {combo_total} method-combo(s), per-method budget allowlists applied")
        for w in workloads:
            print(f"           - {workload_stem(w)}")
        print(f"budgets  : {', '.join(str(b) + 'MB' if b else 'default' for b in budgets)}")
        print(f"total    : {total_runs} run(s)")
        print(f"cooldown : {cooldown}s between runs")
        print(f"hw-reset : {'on (sudo)' if hw_reset else 'off'}")
        print(f"warmup   : {'on (per-db)' if warmup else 'off'}")
        if dry_run:
            print("mode     : dry-run\n")
        else:
            print()

        global_idx = 0
        warmed_up_dbs = set()

        if hw_reset and not dry_run:
            print("[pre-run] initial hw-reset ...")
            reset_hardware(db_path_base)
            if cooldown > 0:
                cooldown_sleep(cooldown)

        for workload in workloads:
            for budget in budgets:
                # Per-budget output subdir: the read CSV name carries only
                # workload+method+params, so budgets would otherwise collide.
                budget_tag = f"mb{budget}" if budget else "default"
                budget_out = os.path.join(output_dir, budget_tag)
                run_env = None
                if budget:
                    run_env = dict(os.environ, EXP_BLOCK_CACHE_MB=str(budget))

                for method in methods:
                    if budget not in method_budgets(method):
                        continue
                    name          = method["name"]
                    method_params = method.get("params", {})
                    query_limit   = method.get("query_limit", 0)
                    combos        = cartesian_combinations(method_params)

                    for combo in combos:
                        global_idx += 1
                        if global_idx < start_from:
                            print(f"[{global_idx}/{total_runs}] [{name}] SKIP (--start-from {start_from})")
                            continue

                        wl_stem = workload_stem(workload)
                        db_path = f"{db_path_base}/{pk_mode}/{name}/{encode_method_params(combo)}"
                        cmd = build_command(name, workload, db_path, budget_out,
                                            combo, common_params, query_limit)

                        env_prefix = f"EXP_BLOCK_CACHE_MB={budget} " if budget else ""
                        print(f"[{global_idx}/{total_runs}] [{budget_tag}][{wl_stem}][{name}] "
                              f"{env_prefix}{' '.join(cmd)}")

                        if not dry_run:
                            if not os.path.exists(db_path):
                                sys.exit(f"DB path does not exist (required for read): {db_path}")
                            os.makedirs(budget_out, exist_ok=True)

                            # Warmup: single read per (db, budget) to populate caches.
                            warm_key = (db_path, budget)
                            if warmup and warm_key not in warmed_up_dbs:
                                print(f"  [warmup] warming up {db_path} @ {budget_tag} ...")
                                with open(workload) as wf:
                                    first_line = wf.readline()
                                with tempfile.NamedTemporaryFile(
                                    mode="w", suffix=".tsv", delete=False
                                ) as tmp:
                                    tmp.write(first_line)
                                    tmp_path = tmp.name
                                try:
                                    warmup_cmd = build_command(
                                        name, tmp_path, db_path, budget_out,
                                        combo, common_params)
                                    rc, _ = run_process(warmup_cmd, env=run_env)
                                    if rc != 0:
                                        sys.exit(f"Warmup failed (exit {rc}): {' '.join(warmup_cmd)}")
                                finally:
                                    os.unlink(tmp_path)
                                warmed_up_dbs.add(warm_key)
                                print(f"  [warmup] done")

                            rc, _ = run_process(cmd, env=run_env)
                            if rc != 0:
                                sys.exit(f"Run failed (exit {rc}): {' '.join(cmd)}")
                            print()

                            if hw_reset:
                                reset_hardware(db_path_base)
                            if cooldown > 0 and global_idx < total_runs:
                                cooldown_sleep(cooldown)

    finally:
        teardown_logging(log_file, log_path)


def main():
    parser = __import__("argparse").ArgumentParser(
        description="NYC Taxi read experiment — block-cache budget sweep"
    )
    add_common_args(parser)
    parser.add_argument("--pk_mode", choices=["uuid", "ulid"], required=True,
                        help="PK regime; selects which DB namespace to read (REQUIRED)")
    parser.add_argument("--warmup", action="store_true",
                        help="Run one warmup query per (DB, budget) before measuring")
    args = parser.parse_args()

    maybe_run_as_daemon(args)

    method_filter = parse_method_filter(args)
    run(args.config, dry_run=args.dry_run, method_filter=method_filter,
        cooldown=args.cooldown, hw_reset=args.hw_reset,
        pk_mode=args.pk_mode, warmup=args.warmup, start_from=args.start_from)


if __name__ == "__main__":
    main()
