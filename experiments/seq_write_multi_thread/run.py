#!/usr/bin/env python3
"""
Multi-threaded sequential write experiment.

Replays a taxi TSV workload with varying num_threads using honk_player.

Usage:
    python3 experiments/seq_write_multi_thread/run.py exp_set/<params>.json [options]
"""

import json
import os
import re
import subprocess
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
    setup_logging,
    teardown_logging,
)

EXP_DIR = os.path.dirname(os.path.abspath(__file__))
BINARY = "build/bin/honk_player"

DB_PARAMS = ["rho"]

SUMMARY_HEADER = "method,num_threads,time_elapsed_ms,records_written,db_size_bytes"


CHECKPOINT_RE = re.compile(r"\[write\]\s+(\d+)\s+records,\s+(\d+)ms")
SUMMARY_TIME_RE = re.compile(r"^Total time:\s*(\d+)ms")
SUMMARY_WRITES_RE = re.compile(r"Total writes:\s*(\d+)")


def run_and_stream_csv(cmd, csv_file, method_label, num_threads):
    """Run honk_player and write checkpoint rows to CSV in real-time.

    Returns (returncode, total_time_ms, total_records).
    """
    proc = subprocess.Popen(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    total_time_ms = None
    total_records = None

    for raw_line in proc.stdout:
        line = raw_line.decode("utf-8", errors="replace").rstrip("\n\r")
        sys.stdout.write(line + "\n")
        sys.stdout.flush()

        # Checkpoint line
        m = CHECKPOINT_RE.search(line)
        if m:
            records, time_ms = int(m.group(1)), int(m.group(2))
            csv_file.write(f"{method_label},{num_threads},{time_ms},{records},\n")
            csv_file.flush()
        # Summary lines
        m = SUMMARY_TIME_RE.search(line)
        if m:
            total_time_ms = int(m.group(1))
        m = SUMMARY_WRITES_RE.search(line)
        if m:
            total_records = int(m.group(1))

    proc.wait()
    return proc.returncode, total_time_ms, total_records


def get_db_size(db_path: str) -> int:
    result = subprocess.run(["du", "-sb", db_path], capture_output=True, text=True)
    return int(result.stdout.split()[0]) if result.returncode == 0 else 0


def encode_method_label(name: str, combo: dict) -> str:
    """Method name for CSV (e.g., 'bitlsm_rho0.1')."""
    db_params = {k: v for k, v in combo.items() if k in DB_PARAMS}
    if db_params:
        return name + "_" + "_".join(f"{k}{fmt(v)}" for k, v in db_params.items())
    return name


def encode_method_params(params: dict) -> str:
    db_params = {k: v for k, v in params.items() if k in DB_PARAMS}
    parts = []
    if db_params:
        parts.append("_".join(f"{k}{fmt(v)}" for k, v in db_params.items()))
    if "num_threads" in params:
        parts.append(f"t{fmt(params['num_threads'])}")
    return "_".join(parts) if parts else "default"


def build_command(method_name: str, workload: str, db_path: str,
                  output_dir: str, combo: dict, common_params: dict) -> list:
    cmd = [BINARY,
           "--binding", method_name,
           "--workload", workload,
           "--db_path", db_path,
           "--output_dir", output_dir,
           "--no_csv"]
    for key, val in common_params.items():
        cmd += [f"--{key}", fmt(val)]
    for key, val in combo.items():
        cmd += [f"--{key}", fmt(val)]
    return cmd


def run(config_path: str, dry_run: bool, method_filter: list,
        cooldown: int, hw_reset: bool, clean_db_flag: bool,
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

    # Extract num_threads from common_params (sweep parameter)
    num_threads_list = common_params.pop("num_threads", [1])
    if not isinstance(num_threads_list, list):
        num_threads_list = [num_threads_list]

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
        total_runs = len(workloads) * method_combos * len(num_threads_list)
        print(f"config   : {config_path}")
        print(f"label    : {exp_label}")
        print(f"total    : {total_runs} run(s)")
        print(f"threads  : {num_threads_list}")
        print(f"cooldown : {cooldown}s between runs")
        print(f"hw-reset : {'on (sudo)' if hw_reset else 'off'}")
        print(f"clean-db : {'on' if clean_db_flag else 'off'}")
        if dry_run:
            print("mode     : dry-run\n")
        else:
            print()

        global_idx = 0

        # Master summary CSV
        summary_csv_path = os.path.join(output_dir, f"{exp_label}.csv")

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
                    for nt in num_threads_list:
                        global_idx += 1
                        if global_idx < start_from:
                            print(f"[{global_idx}/{total_runs}] [{name}] SKIP (--start-from {start_from})")
                            continue

                        full_combo = {**combo, "num_threads": nt}
                        db_path = f"{db_path_base}/{name}/{encode_method_params(full_combo)}"
                        cmd = build_command(name, workload, db_path, output_dir,
                                            full_combo, common_params)

                        print(f"[{global_idx}/{total_runs}] [{name}] {' '.join(cmd)}")

                        if not dry_run:
                            os.makedirs(db_path, exist_ok=True)
                            os.makedirs(output_dir, exist_ok=True)

                            method_label = encode_method_label(name, combo)
                            need_header = not os.path.exists(summary_csv_path)
                            with open(summary_csv_path, "a") as csvf:
                                if need_header:
                                    csvf.write(SUMMARY_HEADER + "\n")
                                rc, time_ms, records = run_and_stream_csv(
                                    cmd, csvf, method_label, nt)
                                if rc != 0:
                                    sys.exit(f"Run failed (exit {rc}): {' '.join(cmd)}")
                                # Final row with db_size
                                db_size = get_db_size(db_path)
                                if time_ms is not None:
                                    csvf.write(f"{method_label},{nt},{time_ms},{records},{db_size}\n")
                                    csvf.flush()
                            if time_ms is not None:
                                print(f"  -> {method_label} t={nt}: {time_ms}ms, {records} records, {db_size} bytes")

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
        description="Multi-threaded sequential write experiment"
    )
    add_common_args(parser)
    args = parser.parse_args()

    maybe_run_as_daemon(args)

    method_filter = parse_method_filter(args)
    run(args.config, dry_run=args.dry_run, method_filter=method_filter,
        cooldown=args.cooldown, hw_reset=args.hw_reset,
        clean_db_flag=args.clean_db, start_from=args.start_from)


if __name__ == "__main__":
    main()
