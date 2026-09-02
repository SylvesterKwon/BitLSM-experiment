"""Measurement-server settings profile (parameter registry §6/§7).

exp_set JSONs override via a top-level "server_common" block; the defaults
here are the registry-recorded values. Knobs are SEMANTIC and expand
symmetrically to both engines — per-engine raw args are deliberately not
exposed: a one-sided setting (e.g. flush_log_at_trx_commit on only one
engine) silently distorts results (registry §7, fsync trap: the LSM
engines drop to ~142 rows/s when only innodb's flush is relaxed).

    "server_common": {
        "cache_bytes": 1073741824,      # buffer pool == block cache
        "direct_io": true,              # bypass OS page cache, both engines
        "relaxed_commit_flush": true,   # flush_log_at_trx_commit=0, both
        "io_capacity_iops": 1000,       # background IO not throttled below
        "lsm_background_jobs": 6         # the device; both engines at their
    }                                   # vendor's current recommendation

Any newly added knob must also be recorded in the parameter registry
(bitlsm-myrocks-docs/experiment-parameter-registry.md).
"""

DEFAULTS = {
    "cache_bytes": 1 << 30,
    "direct_io": True,
    "relaxed_commit_flush": True,
    # Neither engine's background IO may be throttled below what the device
    # can sustain. Both engines ship this knob and default it oppositely:
    # innodb_io_capacity is 200 (a 7200rpm disk), rocksdb's rate_limiter is 0
    # (unlimited). Running both at their defaults compares InnoDB throttled to
    # 0.24% of this NVMe against an unthrottled RocksDB, which is a vendor
    # default-philosophy difference, not an engine difference.
    #
    # 1000 is the MySQL manual's own figure for this device class (17.8.7:
    # "for a higher-end, bus-attached SSD, consider a higher setting such as
    # 1000"), and io_capacity_max follows the manual's paired figure of 2500.
    # The manual also states a 20000 ceiling, but that is a "do not exceed",
    # not a recommendation, so the device-class figure is used instead. The
    # device measures far above either -- 82,839 4KiB random write IOPS at
    # QD=1 on the Samsung 970 EVO Plus -- so this stays a conservative,
    # manual-sanctioned value rather than a device-derived one.
    #
    # RocksDB's counterpart stays at its unlimited default; adding a rate
    # limiter to match a number would only throttle it below the device too.
    "io_capacity_iops": 1000,
    "io_capacity_max_iops": 2500,
    # RocksDB's max_background_jobs caps flushes + compactions together and
    # ships at 2; its own wiki says to "start new DB projects with
    # max_background_jobs = 6", and this repo's honk-side write experiments
    # already run at 6 (experiments/seq_write_multi_thread/exp_set/default.json,
    # chosen after sweeping 2..8 in sweep_bg_jobs.json). Leaving MySQL-side LSM
    # cells at 2 would mean the same engine is configured differently between
    # two experiments in one paper.
    #
    # There is no single InnoDB counterpart: its background work is split
    # across page_cleaners / purge_threads / read_io_threads / write_io_threads,
    # all of which already default to 4 -- current multi-core defaults, unlike
    # io_capacity's documented 7200 RPM figure. So InnoDB needs no change here.
    # This is not "the same number on both sides"; it is each engine at its
    # vendor's current recommendation.
    "lsm_background_jobs": 6,
}

COMMON_ARGS = ["--skip-log-bin", "--character-set-server=latin1",
               "--collation-server=latin1_swedish_ci"]


def resolve(server_common: dict = None) -> dict:
    profile = {**DEFAULTS, **(server_common or {})}
    unknown = set(profile) - set(DEFAULTS)
    if unknown:
        raise ValueError(f"unknown server_common keys: {sorted(unknown)} "
                         f"(known: {sorted(DEFAULTS)})")
    return profile


def build_args(engine: str, profile: dict, write_path: bool = False) -> list:
    """mysqld args for one engine under a resolved profile. write_path=True
    additionally neutralizes the OS page cache on the LSM write path
    (flush/compaction), mirroring innodb O_DIRECT's write-side effect."""
    args = list(COMMON_ARGS)
    if engine == "innodb":
        args.append(f"--innodb-buffer-pool-size={profile['cache_bytes']}")
        if profile["direct_io"]:
            args.append("--innodb-flush-method=O_DIRECT")
        if profile["relaxed_commit_flush"]:
            args.append("--innodb-flush-log-at-trx-commit=0")
        args += [f"--innodb-io-capacity={profile['io_capacity_iops']}",
                 f"--innodb-io-capacity-max="
                 f"{profile['io_capacity_max_iops']}"]
    elif engine in ("myrocks", "bitlsm"):
        args.append(f"--rocksdb-block-cache-size={profile['cache_bytes']}")
        # Index metadata resident from boot, so cold measures data I/O only.
        args.append(
            f"--rocksdb-max-background-jobs={profile['lsm_background_jobs']}")
        args += ["--rocksdb-max-open-files=-1",
                 "--rocksdb-cache-index-and-filter-blocks=0",
                 "--rocksdb-pin-l0-filter-and-index-blocks-in-cache=0"]
        if profile["direct_io"]:
            args.append("--rocksdb-use-direct-reads=1")
            if write_path:
                args.append("--rocksdb-use-direct-io-for-flush-and-compaction=1")
        if profile["relaxed_commit_flush"]:
            args.append("--rocksdb-flush-log-at-trx-commit=0")
        # io_capacity_iops' counterpart: rocksdb_rate_limiter_bytes_per_sec
        # defaults to 0 (unlimited), which is already "not throttled below the
        # device". Left unset on purpose -- see DEFAULTS.
    else:
        raise ValueError(f"unknown engine: {engine}")
    return args
