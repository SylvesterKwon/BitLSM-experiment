#!/usr/bin/env python3
"""
NYC Taxi sequential write experiment.

Replays a taxi TSV workload in write-only mode using honk_player.

After each run exits (drain and Close both done) the DB directory is sized
with `du -sb`, as seq_write does, and appended to db_size.csv in the result
directory: one row per DB, keyed by workload, method and DB_PARAMS.

Usage:
    python3 experiments/nyc_taxi_seq_write/run.py exp_set/<params>.json [options]
"""

import csv
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
    get_db_size_bytes,
    make_result_dir,
    maybe_run_as_daemon,
    parse_method_filter,
    PROJECT_ROOT,
    reset_hardware,
    resolve_workload_paths,
    run_process,
    setup_logging,
    teardown_logging,
    write_run_meta,
)

EXP_DIR = os.path.dirname(os.path.abspath(__file__))
BINARY = os.path.join(PROJECT_ROOT, "build", "bin", "honk_player")

DB_PARAMS = ["rho", "bloom_bits"]
DB_SIZE_COLUMNS = ["workload", "method", *DB_PARAMS, "db_size_bytes"]
# Parameters that name files: a relative value resolves against the project
# root, so a param set can point at a file committed with the experiment.
FILE_PARAMS = ["bin_policy"]


def encode_method_params(params: dict) -> str:
    db_params = {k: v for k, v in params.items() if k in DB_PARAMS}
    if not db_params:
        return "default"
    return "_".join(f"{k}{fmt(v)}" for k, v in db_params.items())


def append_db_size(path: str, row: dict):
    write_header = not os.path.exists(path) or os.path.getsize(path) == 0
    with open(path, "a", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=DB_SIZE_COLUMNS, restval="",
                                extrasaction="ignore")
        if write_header:
            writer.writeheader()
        writer.writerow(row)


def build_command(method_name: str, workload: str, db_path: str,
                  output_dir: str, combo: dict, common_params: dict) -> list:
    cmd = [BINARY,
           "--binding", method_name,
           "--workload", workload,
           "--db_path", db_path,
           "--output_dir", output_dir]
    for key, val in common_params.items():
        cmd += [f"--{key}", fmt(val)]
    for key, val in combo.items():
        if key in FILE_PARAMS and not os.path.isabs(val):
            val = os.path.join(PROJECT_ROOT, val)
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
    workloads     = resolve_workload_paths(config["workload"])
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

        if not dry_run:
            write_run_meta(output_dir, config_path, config, BINARY, log_path)

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
                        os.makedirs(db_path, exist_ok=True)
                        os.makedirs(output_dir, exist_ok=True)

                        rc, _ = run_process(cmd)
                        if rc != 0:
                            sys.exit(f"Run failed (exit {rc}): {' '.join(cmd)}")

                        db_size = get_db_size_bytes(db_path)
                        print(f"  [result] db_size={db_size} bytes")
                        append_db_size(
                            os.path.join(output_dir, "db_size.csv"),
                            {"workload": os.path.splitext(
                                 os.path.basename(workload))[0],
                             "method": name, **combo,
                             "db_size_bytes": db_size})

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
