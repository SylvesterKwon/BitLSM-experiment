"""
Shared utilities for experiment runners (run.py, honk_run.py).

Provides: process execution, logging, hw-reset, cooldown, cartesian product,
CLI argument parsing, and daemon mode.
"""

import argparse
import glob
import os
import subprocess
import sys
import time
from datetime import datetime
from itertools import product

# Project root: parent of src/
PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# ---------------------------------------------------------------------------
# Process management
# ---------------------------------------------------------------------------


def run_process(cmd: list, env: dict = None) -> tuple:
    """Execute a command with real-time output + capture.

    Returns (returncode, captured_output_str).
    """
    proc = subprocess.Popen(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    captured = []
    for raw_line in proc.stdout:
        text = raw_line.decode("utf-8", errors="replace")
        captured.append(text)
        sys.stdout.write(text)
        sys.stdout.flush()
    proc.wait()
    return proc.returncode, "".join(captured)


# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------

class TeeOutput:
    """Write output to both stdout and a log file."""
    def __init__(self, log_file):
        self.log_file = log_file
        self.stdout = sys.stdout

    def write(self, data):
        self.stdout.write(data)
        self.log_file.write(data)
        self.log_file.flush()

    def flush(self):
        self.stdout.flush()
        self.log_file.flush()

    def isatty(self):
        return self.stdout.isatty()


def setup_logging(label: str):
    """Create log directory and tee stdout to a log file.

    Returns (log_file, log_path). Caller must close log_file and restore
    sys.stdout when done.
    """
    log_dir = "logs"
    os.makedirs(log_dir, exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = os.path.join(log_dir, f"{timestamp}_{label}.log")
    try:
        log_file = open(log_path, "w")
        sys.stdout = TeeOutput(log_file)
    except PermissionError:
        log_file = None
        print(f"[warn] cannot write log to {log_path}, logging to stdout only")
    return log_file, log_path


def teardown_logging(log_file, log_path: str):
    """Restore stdout and close log file."""
    if log_file:
        print(f"Log saved to: {log_path}")
        log_file.close()
    sys.stdout = sys.__stdout__


# ---------------------------------------------------------------------------
# Parameter utilities
# ---------------------------------------------------------------------------

def fmt(val) -> str:
    """Format a parameter value to a clean string (no trailing zeros for floats)."""
    if isinstance(val, float):
        return f"{val:g}"
    return str(val)


def cartesian_combinations(params: dict) -> list:
    """Return list of {flag: single_value} dicts from {flag: [values]} input."""
    if not params:
        return [{}]
    keys = list(params.keys())
    values = [params[k] if isinstance(params[k], list) else [params[k]] for k in keys]
    return [dict(zip(keys, combo)) for combo in product(*values)]


# ---------------------------------------------------------------------------
# Hardware reset & cooldown
# ---------------------------------------------------------------------------

def clean_db(db_path: str):
    """Remove a DB directory if it exists."""
    if os.path.exists(db_path):
        print(f"  [clean-db] rm -rf {db_path}")
        subprocess.run(["rm", "-rf", db_path], check=True)


def reset_hardware(db_path_base: str):
    """Reset hardware state between runs: sync, drop_caches, fstrim."""
    print("  [hw-reset] sync + drop_caches ...")
    subprocess.run(["sync"], check=True)
    subprocess.run(["sudo", "tee", "/proc/sys/vm/drop_caches"],
                   input=b"3", stdout=subprocess.DEVNULL, check=True)

    os.makedirs(db_path_base, exist_ok=True)
    print("  [hw-reset] fstrim ...")
    subprocess.run(["sudo", "fstrim", "-v", db_path_base], check=True)

    print("  [hw-reset] waiting 10s for SSD GC ...")
    time.sleep(10)
    print("  [hw-reset] done")


def warn_if_cpu_unpinned():
    """Print a warning when the CPU clock is free to drift (read-only check).

    An unpinned clock swings with temperature across runs and drags wall clock
    with it, which is how an ingest sweep once reported a layout as faster than
    the baseline it does strictly more work than. Both settings are lost on
    reboot, so this is a per-boot trap worth catching before a run rather than
    after. See README, Measurement hygiene.
    """
    problems = []
    govs = glob.glob("/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor")
    seen = set()
    for path in govs:
        try:
            with open(path) as f:
                seen.add(f.read().strip())
        except OSError:
            pass
    if seen and seen != {"performance"}:
        problems.append(f"governor is {'/'.join(sorted(seen))}, want performance")
    try:
        with open("/sys/devices/system/cpu/intel_pstate/no_turbo") as f:
            if f.read().strip() != "1":
                problems.append("turbo is on, want it off")
    except OSError:
        pass  # not intel_pstate; nothing to assert
    if not problems:
        return
    print("  [WARNING] CPU clock is not pinned — timings will drift between "
          "runs:")
    for p in problems:
        print(f"            {p}")
    print("            echo performance | sudo tee "
          "/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor")
    print("            echo 1 | sudo tee "
          "/sys/devices/system/cpu/intel_pstate/no_turbo")


def make_result_dir(exp_name: str) -> str:
    """Build a timestamped result directory under PROJECT_ROOT/results/.

    Returns: absolute path like ``results/202604061430_seq_write/``.
    The directory is NOT created here — callers create it with ``os.makedirs``.
    """
    ts = datetime.now().strftime("%Y%m%d_%H%M")
    return os.path.join(PROJECT_ROOT, "results", f"{ts}_{exp_name}")


def resolve_workload_paths(raw) -> list:
    """Normalize a config's ``workload`` entry into a list of absolute paths.

    Accepts a single path or a list of them. Relative paths are resolved
    against PROJECT_ROOT (not the caller's cwd), so param sets can be written
    machine-independently as ``workloads/foo.tsv``. Absolute paths pass
    through unchanged.
    """
    paths = raw if isinstance(raw, list) else [raw]
    return [
        p if os.path.isabs(p) else os.path.normpath(os.path.join(PROJECT_ROOT, p))
        for p in paths
    ]


def cooldown_sleep(seconds: int):
    """Sleep with countdown display."""
    for remaining in range(seconds, 0, -1):
        print(f"\r  [cooldown] {remaining}s... ", end="", flush=True)
        time.sleep(1)
    print(f"\r  [cooldown] done          ")


# ---------------------------------------------------------------------------
# CLI argument helpers
# ---------------------------------------------------------------------------

def add_common_args(parser: argparse.ArgumentParser):
    """Add CLI arguments shared by all experiment runners."""
    parser.add_argument("config", help="Path to params.json")
    parser.add_argument("--dry-run", action="store_true",
                        help="Print commands without executing")
    parser.add_argument("--methods",
                        help="Comma-separated list of methods to run (default: all)")
    parser.add_argument("--cooldown", type=int, default=0, metavar="SECONDS",
                        help="Seconds to wait between runs (default: 0)")
    parser.add_argument("--hw-reset", action="store_true",
                        help="Reset hardware state between runs (sudo required)")
    parser.add_argument("--clean-db", action="store_true",
                        help="Delete DB directories before each write run")
    parser.add_argument("--start-from", type=int, default=1, metavar="N",
                        help="Start from the N-th experiment (1-indexed, default: 1)")
    parser.add_argument("--daemon", action="store_true", default=True,
                        help="Run in background via nohup (default: on)")
    parser.add_argument("--no-daemon", dest="daemon", action="store_false",
                        help="Run in foreground (disable daemon mode)")


def maybe_run_as_daemon(args):
    """If --daemon is set, re-exec via nohup and exit. Otherwise return."""
    if not args.daemon:
        return
    child_argv = [a for a in sys.argv if a not in ("--daemon", "--no-daemon")]
    child_argv.append("--no-daemon")
    cmd = ["nohup", sys.executable] + child_argv
    proc = subprocess.Popen(
        cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        preexec_fn=os.setpgrp)

    # Write PID file so the process can be killed easily later.
    pid_dir = os.path.join(PROJECT_ROOT, "logs")
    os.makedirs(pid_dir, exist_ok=True)
    pid_path = os.path.join(pid_dir, f"{proc.pid}.pid")
    with open(pid_path, "w") as f:
        f.write(f"{proc.pid}\n{' '.join(cmd)}\n")

    print(f"Background PID: {proc.pid}")
    print(f"  PID file: {pid_path}")
    print(f"  Check:    ps -p {proc.pid}")
    print(f"  Kill:     kill {proc.pid} && rm {pid_path}")
    sys.exit(0)


def parse_method_filter(args) -> list:
    """Parse --methods flag into a list (empty = all)."""
    return args.methods.split(",") if args.methods else []
