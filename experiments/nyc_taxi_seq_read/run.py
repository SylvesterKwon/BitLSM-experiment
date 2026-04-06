#!/usr/bin/env python3
"""
NYC Taxi sequential read experiment.

Replays taxi TSV read workloads using honk_player. DB must already exist
(built by nyc_taxi_seq_write).

Usage:
    python3 experiments/nyc_taxi_seq_read/run.py exp_set/<params>.json [options]
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
    reset_hardware,
    run_process,
    setup_logging,
    teardown_logging,
)

EXP_DIR = os.path.dirname(os.path.abspath(__file__))
BINARY = "build/bin/honk_player"

DB_PARAMS = ["rho"]


def encode_method_params(params: dict) -> str:
    db_params = {k: v for k, v in params.items() if k in DB_PARAMS}
    if not db_params:
        return "default"
    return "_".join(f"{k}{fmt(v)}" for k, v in db_params.items())


def workload_stem(path: str) -> str:
    return os.path.splitext(os.path.basename(path))[0]


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
        cmd += [f"--{key}", fmt(val)]
    return cmd


def run(config_path: str, dry_run: bool, method_filter: list,
        cooldown: int, hw_reset: bool, warmup: bool = False,
        start_from: int = 1):
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
        print(f"axes     : {len(workloads)} workload(s) x {method_combos} method-combo(s)")
        for w in workloads:
            print(f"           - {workload_stem(w)}")
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
            for method in methods:
                name          = method["name"]
                method_params = method.get("params", {})
                combos        = cartesian_combinations(method_params)

                for combo in combos:
                    global_idx += 1
                    if global_idx < start_from:
                        print(f"[{global_idx}/{total_runs}] [{name}] SKIP (--start-from {start_from})")
                        continue

                    wl_stem = workload_stem(workload)
                    db_path = f"{db_path_base}/{name}/{encode_method_params(combo)}"
                    cmd = build_command(name, workload, db_path, output_dir,
                                        combo, common_params)

                    print(f"[{global_idx}/{total_runs}] [{wl_stem}][{name}] {' '.join(cmd)}")

                    if not dry_run:
                        if not os.path.exists(db_path):
                            sys.exit(f"DB path does not exist (required for read): {db_path}")
                        os.makedirs(output_dir, exist_ok=True)

                        # Warmup: single read per db_path to populate OS page cache
                        if warmup and db_path not in warmed_up_dbs:
                            print(f"  [warmup] warming up {db_path} ...")
                            with open(workload) as wf:
                                first_line = wf.readline()
                            with tempfile.NamedTemporaryFile(
                                mode="w", suffix=".tsv", delete=False
                            ) as tmp:
                                tmp.write(first_line)
                                tmp_path = tmp.name
                            try:
                                warmup_cmd = build_command(
                                    name, tmp_path, db_path, output_dir,
                                    combo, common_params)
                                rc, _ = run_process(warmup_cmd)
                                if rc != 0:
                                    sys.exit(f"Warmup failed (exit {rc}): {' '.join(warmup_cmd)}")
                            finally:
                                os.unlink(tmp_path)
                            warmed_up_dbs.add(db_path)
                            print(f"  [warmup] done")

                        rc, _ = run_process(cmd)
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
        description="NYC Taxi sequential read experiment"
    )
    add_common_args(parser)
    parser.add_argument("--warmup", action="store_true",
                        help="Run one warmup query per DB before measuring")
    args = parser.parse_args()

    maybe_run_as_daemon(args)

    method_filter = parse_method_filter(args)
    run(args.config, dry_run=args.dry_run, method_filter=method_filter,
        cooldown=args.cooldown, hw_reset=args.hw_reset,
        warmup=args.warmup, start_from=args.start_from)


if __name__ == "__main__":
    main()
