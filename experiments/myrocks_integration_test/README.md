# myrocks_integration_test — SQL-level integration tests for BitLSM-embedded MyRocks

End-to-end benchmark of the BitLSM index inside MyRocks (MySQL 8.0.32
fb-mysql fork, submodule `third_party/BitLSM-mysql-5.6`) against InnoDB and
plain MyRocks, at the SQL level: index selection (optimizer `auto` vs hinted
plans), cardinality-estimator quality (q-error vs cached ground truth), read
latency, and ingest throughput.

## Prerequisites

```bash
git submodule update --init --recursive          # BitLSM nests rocksdb
scripts/build_mysqld.sh release                  # -> build-mysql/release/
```

Perf/ingest runs require the release build; plan-mode also accepts debug.

## Datasets

Both are single-table workloads behind the same `Workload` interface
(`src/myrocks/workloads/`); runners resolve them via `registry.py`.

| workload | table | rows | procurement |
|---|---|---|---|
| `ssbflat` (SF n) | `lineorder_flat` (42 cols) | 6.0M × SF | **generated** — ssb-dbgen submodule + streaming join, auto-built on first load (`workloads/sql/ssb_flat/README.md`) |
| `pbi_taxpayer` (instance 2) | `taxpayer` (28 cols + rid) | 9,153,273 | **frozen** — explicit download + pin: `python3 scripts/prepare_taxpayer.py 2` (Zenodo, md5-verified, sha256-pinned in `workloads/sql/publicbi_taxpayer/MANIFEST.json`; see that directory's README) |

SSB data materializes on first `ensure_loaded()`; Taxpayer data must be
prepared explicitly first — the harness only verifies against the committed
MANIFEST and never downloads.

## Runners

All take an exp_set JSON + the common runner flags (`--dry-run`,
`--start-from`, `--no-daemon`, ...; daemon is the default, logs in `logs/`).

```bash
# plan-mode: EXPLAIN/optimizer-trace sweep + COUNT ground-truth gate
python3 experiments/myrocks_integration_test/run.py \
    experiments/myrocks_integration_test/exp_set/taxpayer_plan.json

# perf-mode: cold+warm latency, counters, result-fingerprint gate (release only)
python3 experiments/myrocks_integration_test/perf_run.py \
    experiments/myrocks_integration_test/exp_set/taxpayer_perf.json

# ingest: row-streaming write throughput (SSB-only for now)
python3 experiments/myrocks_integration_test/ingest_run.py \
    experiments/myrocks_integration_test/exp_set/ssb_flat_h4_endurance.json
```

Datadirs are identity-cached under `data/sql/db/<workload>-<layout>-<engine>`
(built once by `ensure_loaded()`, marked by `LOADED.json`, LSM state
compacted). Delete a datadir to force a reload.

## Correctness gates (any mismatch fails the sweep)

- **COUNT gate** (plan-mode): every engine's `SELECT COUNT(*)` per query
  WHERE is checked against a cached engine-independent ground truth
  (`ground_truth.<workload>.json` next to the data).
- **Fingerprint gate** (perf-mode): order-independent md5 over full result
  sets, compared across engines and plans (floats normalized to 12
  significant digits).

## Results

`results/YYYYMMDD_HHMM_myrocks_integration_test_<exp_set>/` —
`plan_mode.csv` / `perf_mode.csv` + per-cell trace/counter sidecars.
