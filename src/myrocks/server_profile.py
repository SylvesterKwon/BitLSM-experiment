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
        "relaxed_commit_flush": true    # flush_log_at_trx_commit=0, both
    }

Any newly added knob must also be recorded in the parameter registry
(bitlsm-myrocks-docs/experiment-parameter-registry.md).
"""

DEFAULTS = {
    "cache_bytes": 1 << 30,
    "direct_io": True,
    "relaxed_commit_flush": True,
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
    elif engine in ("myrocks", "bitlsm"):
        args.append(f"--rocksdb-block-cache-size={profile['cache_bytes']}")
        if profile["direct_io"]:
            args.append("--rocksdb-use-direct-reads=1")
            if write_path:
                args.append("--rocksdb-use-direct-io-for-flush-and-compaction=1")
        if profile["relaxed_commit_flush"]:
            args.append("--rocksdb-flush-log-at-trx-commit=0")
    else:
        raise ValueError(f"unknown engine: {engine}")
    return args
