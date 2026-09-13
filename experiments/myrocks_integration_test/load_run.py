#!/usr/bin/env python3
"""Build the datadirs an exp_set's cells need, and stop.

Nothing here is a measurement. A fixture is an input to the read sweeps, and
this runner's only job is to produce it as cheaply as it can -- loader.py
therefore takes whatever shortcut each engine offers, such as building
InnoDB's secondary indexes by sort after the load rather than by insertion
during it. Wall-clock times printed below are progress reporting, not results,
and must not be quoted as write performance: the write axis is measured by
ingest_run.py, which loads row by row through the client and is deliberately
left alone.

It gets its own runner because a fixture is shared and long-lived. read_plan
and read_perf carry the same cell axis and therefore the same fixtures, and a
fixture outlives the sweeps that read it. The measurement runners still call
ensure_loaded and still build what is missing -- this does not become a step
you can forget -- but with the fixtures already in place that call returns
immediately and they do nothing but measure.

Identity, not path: a datadir's name is derived from (workload, layout,
engine, engine_params), so a cell cannot be pointed at the wrong fixture and
two scale factors coexist under their own names.

Usage:
    python3 experiments/myrocks_integration_test/load_run.py exp_set/<params>.json
    python3 experiments/myrocks_integration_test/load_run.py exp_set/<params>.json --dry-run
"""

import json
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "src"))

from run_common import (  # noqa: E402
    add_common_args, maybe_run_as_daemon, setup_logging, teardown_logging,
)
from myrocks.loader import (  # noqa: E402
    check_fixture_build, datadir_for, ensure_loaded, is_loaded,
)
from myrocks.workloads.registry import make_workload  # noqa: E402


def dir_bytes(path):
    total = 0
    for root, _, files in os.walk(path):
        for name in files:
            try:
                total += os.path.getsize(os.path.join(root, name))
            except OSError:
                pass
    return total


def fixtures(config):
    """The distinct datadirs an exp_set needs. Cells sharing an identity
    (same engine/layout/params, differing only in plans or session_vars)
    collapse to one fixture."""
    workload = make_workload(config)
    seen = {}
    for cell in config["cells"]:
        identity = workload.identity(cell["engine"], cell["index_layout"],
                                     cell.get("engine_params"))
        seen.setdefault(identity, (cell["engine"], cell["index_layout"],
                                   cell.get("engine_params")))
    return workload, seen


def run(config_path, dry_run, start_from):
    with open(config_path) as f:
        config = json.load(f)
    build_kind = config.get("build_kind", "debug")
    workload, wanted = fixtures(config)

    exp_set = os.path.splitext(os.path.basename(config_path))[0]
    exp_name = os.path.basename(os.path.dirname(os.path.abspath(__file__)))
    log_file, log_path = setup_logging(f"{exp_name}_{exp_set}_load")
    try:
        print(f"config : {config_path}")
        print(f"build  : {build_kind}")
        print(f"fixtures: {len(wanted)}")
        pending = [i for i in wanted if not is_loaded(i)]
        for identity in wanted:
            state = "cached" if is_loaded(identity) else "NEEDS LOAD"
            print(f"  [{state:>10}] {identity}")
            print(f"               {datadir_for(identity)}")
            check_fixture_build(identity, build_kind)
        if dry_run:
            print(f"dry-run: {len(pending)} of {len(wanted)} need building")
            return 0
        if not pending:
            print("nothing to build — every fixture is already loaded")
            return 0

        started = time.time()
        for idx, (identity, (engine, layout, eparams)) in enumerate(
                wanted.items(), 1):
            if idx < start_from:
                continue
            if is_loaded(identity):
                print(f"[{idx}/{len(wanted)}] {identity}: cached", flush=True)
                continue
            print(f"[{idx}/{len(wanted)}] {identity}: building ...",
                  flush=True)
            t0 = time.time()
            ensure_loaded(workload, engine, layout, build_kind, eparams)
            size = dir_bytes(datadir_for(identity))
            print(f"[{idx}/{len(wanted)}] {identity}: "
                  f"{time.time() - t0:.0f}s, {size / (1 << 30):.1f} GiB",
                  flush=True)
        print(f"done: {len(wanted)} fixtures ready "
              f"in {time.time() - started:.0f}s")
        return 0
    finally:
        teardown_logging(log_file, log_path)


def main():
    import argparse
    parser = argparse.ArgumentParser(
        description="Build the datadirs an exp_set needs")
    add_common_args(parser)
    args = parser.parse_args()
    maybe_run_as_daemon(args)
    sys.exit(run(args.config, dry_run=args.dry_run,
                 start_from=args.start_from))


if __name__ == "__main__":
    main()
