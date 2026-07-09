#!/usr/bin/env python3
"""
NYC Taxi sequential write experiment.

Replays a taxi TSV workload in write-only mode using honk_player.

Usage:
    python3 experiments/nyc_taxi_seq_write/run.py exp_set/<params>.json [options]
"""

import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "src"))

from run_common import (
    add_common_args,
    cartesian_combinations,
    clean_db,
    cooldown_sleep,
    fmt,
    make_result_dir,
    maybe_run_as_daemon,
    parse_method_filter,
    reset_hardware,
    run_process,
    setup_logging,
    teardown_logging,
)

EXP_DIR = os.path.dirname(os.path.abspath(__file__))
BINARY = "build/bin/honk_player"

# DB-identity params: encoded into the db_path so a read reopens the SAME DB
# the write built. "rho" (bitlsm), "bloom_bits" (embedded), and "m" (next,
# Task 5.3) each change what is persisted on disk, so each must namespace the
# db_path. "m" sets next's global-index granularity (E_i = N/(m*B)); a bigger m
# builds a coarser index, so an m-sweep must live in separate db_paths.
DB_PARAMS = ["rho", "bloom_bits", "m"]


def encode_method_params(params: dict) -> str:
    db_params = {k: v for k, v in params.items() if k in DB_PARAMS}
    if not db_params:
        return "default"
    return "_".join(f"{k}{fmt(v)}" for k, v in db_params.items())


def build_command(method_name: str, workload: str, db_path: str,
                  output_dir: str, combo: dict, common_params: dict) -> list:
    if method_name == "next":
        # NEXT (unmodified RocksDB 7.7.3 fork) uses its own driver, isolated
        # from honk_player's RocksDB 10.10.0 build -- see src/next_driver/.
        # --indexed_attrs (plural) takes a comma-joined list of N co-resident
        # indexed attrs (continuous + categorical); --mode write is this
        # runner's half of the write/read process split (Task 3.1/3.4).
        params = {**common_params, **combo}
        cmd = ["build/bin/next_honk",
               "--workload", workload,
               "--db_path", db_path,
               "--output_dir", output_dir,
               "--indexed_attrs", str(params["indexed_attrs"]),
               "--mode", "write"]
        # Task 5.2/5.3: --m is the single global-index granularity knob. In
        # write mode the driver's stats pre-pass derives each attr's gap
        # threshold delta_i to hit E_i = N/(m*B) entries, so --m shapes the DB
        # built here. "m" is in DB_PARAMS, so it also namespaces db_path above.
        if "m" in params:
            cmd += ["--m", fmt(params["m"])]
        if "max_background_jobs" in params:
            cmd += ["--max_background_jobs", fmt(params["max_background_jobs"])]
        return cmd

    cmd = [BINARY,
           "--binding", method_name,
           "--workload", workload,
           "--db_path", db_path,
           "--output_dir", output_dir]
    for key, val in common_params.items():
        cmd += [f"--{key}", fmt(val)]
    for key, val in combo.items():
        cmd += [f"--{key}", fmt(val)]
    return cmd


def run(config_path: str, dry_run: bool, method_filter: list,
        cooldown: int, hw_reset: bool, clean_db_flag: bool,
        pk_mode: str, start_from: int = 1):
    with open(config_path) as f:
        config = json.load(f)

    exp_set_name  = os.path.splitext(os.path.basename(config_path))[0]
    exp_name      = os.path.basename(EXP_DIR)
    exp_label     = f"{exp_name}_{exp_set_name}"
    db_path_base  = config["db_path_base"]
    raw_workload  = config["workload"]
    workloads     = raw_workload if isinstance(raw_workload, list) else [raw_workload]
    methods       = config["methods"]
    common_params = config.get("common_params", {})
    output_dir    = make_result_dir(exp_label)

    log_file, log_path = setup_logging(exp_label)

    try:
        if method_filter:
            methods = [m for m in methods if m["name"] in method_filter]
            if not methods:
                sys.exit(f"No methods matched: {method_filter}")

        method_combos = sum(
            len(cartesian_combinations(m.get("params", {})))
            for m in methods
        )
        total_runs = len(workloads) * method_combos
        print(f"config   : {config_path}")
        print(f"label    : {exp_label}")
        print(f"total    : {total_runs} run(s)")
        print(f"cooldown : {cooldown}s between runs")
        print(f"hw-reset : {'on (sudo)' if hw_reset else 'off'}")
        print(f"clean-db : {'on' if clean_db_flag else 'off'}")
        if dry_run:
            print("mode     : dry-run\n")
        else:
            print()

        global_idx = 0

        if hw_reset and not dry_run:
            print("[pre-run] initial hw-reset ...")
            reset_hardware(db_path_base)
            if cooldown > 0:
                cooldown_sleep(cooldown)

        for workload in workloads:
            for method in methods:
                name          = method["name"]
                method_params = method.get("params", {})
                combos        = cartesian_combinations(method_params)

                for combo in combos:
                    global_idx += 1
                    if global_idx < start_from:
                        print(f"[{global_idx}/{total_runs}] [{name}] SKIP (--start-from {start_from})")
                        continue

                    db_path = f"{db_path_base}/{pk_mode}/{name}/{encode_method_params(combo)}"
                    cmd = build_command(name, workload, db_path, output_dir,
                                        combo, common_params)

                    print(f"[{global_idx}/{total_runs}] [{name}] {' '.join(cmd)}")

                    if not dry_run:
                        # next_honk --mode write builds each categorical
                        # indexed column's dictionary ids FRESH, in first-seen
                        # order, every run (Task 3.3-m4 carry-over). Re-running
                        # write against a db_path that already holds records
                        # would reassign ids and desync them from the
                        # already-persisted values -- silently corrupting
                        # categorical reads. This runner's "build DB for a
                        # later read" mode intentionally does NOT pass
                        # --clean-db (that deletes the DB AFTER the run,
                        # which would defeat nyc_taxi_seq_read), so refuse
                        # outright rather than risk a silent desync.
                        if (name == "next" and os.path.isdir(db_path)
                                and os.listdir(db_path)):
                            sys.exit(
                                f"REFUSING to run 'next' write against a "
                                f"NON-FRESH db_path (already contains files): "
                                f"{db_path}\nnext_honk's categorical "
                                f"dictionary ids are assigned fresh on every "
                                f"write pass; re-running write here would "
                                f"desync ids from already-persisted records. "
                                f"Remove the directory first (rm -rf "
                                f"{db_path}).")
                        os.makedirs(db_path, exist_ok=True)
                        os.makedirs(output_dir, exist_ok=True)

                        rc, _ = run_process(cmd)
                        if rc != 0:
                            sys.exit(f"Run failed (exit {rc}): {' '.join(cmd)}")

                        if clean_db_flag:
                            clean_db(db_path)
                        print()

                        if hw_reset:
                            reset_hardware(db_path_base)
                        if cooldown > 0 and global_idx < total_runs:
                            cooldown_sleep(cooldown)

    finally:
        teardown_logging(log_file, log_path)


def main():
    parser = __import__("argparse").ArgumentParser(
        description="NYC Taxi sequential write experiment"
    )
    add_common_args(parser)
    parser.add_argument("--pk_mode", choices=["uuid", "ulid"], required=True,
                        help="PK regime; namespaces the DB path (REQUIRED)")
    args = parser.parse_args()

    maybe_run_as_daemon(args)

    method_filter = parse_method_filter(args)
    run(args.config, dry_run=args.dry_run, method_filter=method_filter,
        cooldown=args.cooldown, hw_reset=args.hw_reset,
        clean_db_flag=args.clean_db, pk_mode=args.pk_mode,
        start_from=args.start_from)


if __name__ == "__main__":
    main()
