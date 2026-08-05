#!/usr/bin/env python3
"""
Sequential write experiment.

Measures write time and DB size for each method × parameter combination.

Usage:
    python3 experiments/seq_write/run.py exp_set/<params>.json [options]

Options:
    --dry-run             Print commands without executing.
    --methods m1,m2       Run only the specified methods (comma-separated).
    --cooldown SECONDS    Wait between runs (default: 0).
    --hw-reset            Reset hardware state between runs (sudo required).
    --clean-db            Delete DB directories before each write run.
    --start-from N        Start from the N-th experiment (1-indexed).
    --daemon / --no-daemon  Background mode (default: daemon).
"""

import argparse
import csv
import json
import os
import subprocess
import sys

# Add src/ to path so we can import run_common
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

# Parameters that define DB identity.
DB_PARAMS = ["n", "schema", "rho"]

MASTER_COLUMNS = [
    "method", "time_elapsed_ms", "records_written", "db_size_bytes",
]


def _path_safe(key: str, val) -> str:
    if key == "schema":
        return f"{key}_{os.path.splitext(os.path.basename(str(val)))[0]}"
    return f"{key}{fmt(val)}"


def encode_params(params: dict) -> str:
    return "_".join(_path_safe(k, v) for k, v in params.items())


def _schema_stem(schema_path: str) -> str:
    return os.path.splitext(os.path.basename(schema_path))[0]


def get_db_size_bytes(db_path: str) -> int:
    """Get total size of a DB directory in bytes using du."""
    result = subprocess.run(
        ["du", "-sb", db_path], capture_output=True, text=True)
    if result.returncode == 0:
        return int(result.stdout.split()[0])
    return -1


def parse_checkpoints(output: str) -> list[tuple[int, int]]:
    """Parse checkpoint lines from benchmark binary stdout.

    Looks for lines like: 'putted: 1000000 kvps, elapsed: 1234ms'
    and final line: 'created 100000000 kvps. (total:12345ms elapsed)'

    Returns list of (time_elapsed_ms, records_written).
    """
    checkpoints = []
    for line in output.splitlines():
        if "putted:" in line and "elapsed:" in line:
            parts = line.split()
            records = int(parts[parts.index("putted:") + 1])
            elapsed_str = parts[-1]  # e.g. "1234ms"
            elapsed = int(elapsed_str.rstrip("ms"))
            checkpoints.append((elapsed, records))
        elif "total:" in line and "ms elapsed" in line:
            start = line.index("total:") + len("total:")
            end = line.index("ms elapsed")
            total_ms = int(line[start:end])
            # Extract n from "created N kvps."
            parts = line.split()
            n = int(parts[parts.index("created") + 1])
            checkpoints.append((total_ms, n))
    return checkpoints


def method_label(name: str, combo: dict) -> str:
    """Build method label, appending rho for bitlsm (e.g. 'bitlsm_rho0.01')."""
    if "rho" in combo:
        return f"{name}_rho{fmt(combo['rho'])}"
    return name


def build_command(binary: str, exp_label: str, db_path: str,
                  output_dir: str, combo: dict) -> list:
    cmd = [binary,
           "--exp_label", exp_label,
           "--exp_type", "write_seq",
           "--db_path", db_path,
           "--output_dir", output_dir]
    for key, val in combo.items():
        prefix = "-" if len(key) == 1 else "--"
        cmd += [f"{prefix}{key}", fmt(val)]
    return cmd


def append_result(master_path: str, row: dict):
    write_header = (not os.path.exists(master_path)
                    or os.path.getsize(master_path) == 0)
    with open(master_path, "a", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=MASTER_COLUMNS, restval="")
        if write_header:
            writer.writeheader()
        writer.writerow(row)


def run(config_path: str, dry_run: bool, method_filter: list, cooldown: int,
        hw_reset: bool, clean_db_flag: bool, start_from: int = 1):
    with open(config_path) as f:
        config = json.load(f)

    exp_set_name = os.path.splitext(os.path.basename(config_path))[0]
    exp_name     = os.path.basename(EXP_DIR)
    exp_label    = f"{exp_name}_{exp_set_name}"
    db_path_base = config["db_path_base"]
    common       = config.get("common_params", {})
    methods      = config["methods"]
    schema_dir   = config.get("schema_dir", "src/benchmark/schema")

    # This experiment always runs sequential writes — no exp_type in params.json.

    # Resolve schema filenames to full paths (relative to project root)
    project_root = os.path.join(EXP_DIR, "..", "..")
    if "schema" in common:
        schemas = common["schema"] if isinstance(common["schema"], list) else [common["schema"]]
        common["schema"] = [
            os.path.normpath(os.path.join(project_root, schema_dir, s))
            for s in schemas
        ]

    # Per-method progress CSV goes alongside the benchmark's own CSV
    output_dir = make_result_dir(exp_label)
    master_csv = os.path.join(output_dir, f"{exp_label}.csv")

    log_file, log_path = setup_logging(exp_label)

    try:
        if method_filter:
            methods = [m for m in methods if m["name"] in method_filter]
            if not methods:
                sys.exit(f"No methods matched: {method_filter}")

        total_runs = sum(
            len(cartesian_combinations({**common, **m.get("params", {})}))
            for m in methods
        )
        print(f"config   : {config_path}")
        print(f"label    : {exp_label}")
        print(f"total    : {total_runs} run(s) across {len(methods)} method(s)")
        print(f"cooldown : {cooldown}s between runs")
        print(f"hw-reset : {'on (sudo)' if hw_reset else 'off'}")
        print(f"clean-db : {'on' if clean_db_flag else 'off'}")
        print(f"master   : {master_csv}")
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

        for method in methods:
            name          = method["name"]
            binary        = method["binary"]
            method_params = method.get("params", {})

            all_params = {**common, **method_params}
            combos     = cartesian_combinations(all_params)

            for combo in combos:
                global_idx += 1
                if global_idx < start_from:
                    print(f"[{global_idx}/{total_runs}] [{name}] SKIP (--start-from {start_from})")
                    continue

                db_combo = {k: v for k, v in combo.items() if k in DB_PARAMS}
                db_path = f"{db_path_base}/{name}/{encode_params(db_combo)}"
                cmd = build_command(binary, exp_label, db_path, output_dir, combo)

                print(f"[{global_idx}/{total_runs}] [{name}] {' '.join(cmd)}")

                if not dry_run:
                    os.makedirs(db_path, exist_ok=True)
                    os.makedirs(output_dir, exist_ok=True)

                    rc, captured = run_process(cmd)
                    if rc != 0:
                        sys.exit(f"Run failed (exit {rc}): {' '.join(cmd)}")

                    checkpoints = parse_checkpoints(captured)
                    db_size = get_db_size_bytes(db_path)
                    label = method_label(name, combo)

                    if checkpoints:
                        total_ms = checkpoints[-1][0]
                        print(f"  [result] total_time={total_ms}ms, db_size={db_size} bytes ({db_size / (1024**3):.2f} GiB)")

                    if clean_db_flag:
                        clean_db(db_path)
                    print()

                    for elapsed_ms, records in checkpoints[:-1]:
                        append_result(master_csv, {
                            "method": label,
                            "time_elapsed_ms": elapsed_ms,
                            "records_written": records,
                            "db_size_bytes": "",
                        })
                    if checkpoints:
                        append_result(master_csv, {
                            "method": label,
                            "time_elapsed_ms": checkpoints[-1][0],
                            "records_written": checkpoints[-1][1],
                            "db_size_bytes": db_size,
                        })

                    if hw_reset:
                        reset_hardware(db_path_base)
                    if cooldown > 0 and global_idx < total_runs:
                        cooldown_sleep(cooldown)

    finally:
        teardown_logging(log_file, log_path)


def main():
    parser = argparse.ArgumentParser(
        description="Sequential write performance: measure write time and DB size"
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
