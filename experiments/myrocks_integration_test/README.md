# myrocks_integration_test — SQL-level integration tests for BitLSM-embedded MyRocks

End-to-end benchmark of the BitLSM index inside MyRocks (MySQL 8.0.32
fb-mysql fork, submodule `third_party/BitLSM-mysql-5.6`) against InnoDB and
plain MyRocks, at the SQL level: index selection (optimizer `auto` vs hinted
plans), cardinality-estimator quality (q-error vs cached ground truth), read
latency, and ingest throughput.

## Installation

Host packages and the Python client the runners import:

```bash
sudo apt install -y bison libncurses-dev
pip install mysql-connector-python
```

Submodules. The chain is mysql -> BitLSM -> facebook/rocksdb; the build
needs all three levels:

```bash
git submodule update --init third_party/BitLSM-mysql-5.6
git -C third_party/BitLSM-mysql-5.6 submodule update --init rocksdb
git -C third_party/BitLSM-mysql-5.6/rocksdb submodule update --init third_party/rocksdb
git submodule update --init third_party/ssb-dbgen        # SSB workloads only
```

Boost 1.77.0, placed by hand before configuring. MySQL 8.0.32 downloads it
from `boostorg.jfrog.io`, which is shut down — cmake gets an HTML error page,
accepts it as the tarball, and dies at extraction:

```bash
mkdir -p ~/Repositories/.mysql-boost && cd ~/Repositories/.mysql-boost
curl -fLO https://archives.boost.io/release/1.77.0/source/boost_1_77_0.tar.bz2
tar xjf boost_1_77_0.tar.bz2      # cmake skips its download once this dir exists
```

Override the location with `MYSQL_BOOST_DIR` if you keep boost elsewhere.

Build. Perf and ingest require `release`; plan-mode also accepts `debug`:

```bash
scripts/build_mysqld.sh release   # -> build-mysql/release/
```

Data. SSB materializes itself on first load; Taxpayer is fetched explicitly
and verified against the committed MANIFEST:

```bash
python3 scripts/prepare_taxpayer.py 2   # 384MB download -> 1.9GB prepared
```

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
    experiments/myrocks_integration_test/exp_set/taxpayer_read_plan.json

# perf-mode: cold+warm latency, counters, result-fingerprint gate (release only)
python3 experiments/myrocks_integration_test/perf_run.py \
    experiments/myrocks_integration_test/exp_set/taxpayer_read_perf.json

# ingest: row-streaming write throughput (the index-maintenance axis)
python3 experiments/myrocks_integration_test/ingest_run.py \
    experiments/myrocks_integration_test/exp_set/taxpayer_write_perf.json

# estimator A/B: same cell with the M5 estimator on vs the magic fallback
python3 experiments/myrocks_integration_test/run.py \
    experiments/myrocks_integration_test/exp_set/ssb_flat_read_plan_estimator_off.json
```

## exp_set map

A dataset is just a variable: every set exists for both (`taxpayer_*` and
`ssb_flat_*`) and the read sets carry an identical cell axis, so the two
tables can be read side by side.

**`<ds>_read_plan`** (`run.py`) — what the optimizer *decides*. Per cell and
per plan it records the chosen access path, the estimated row count, the
q-error against a cached ground truth, and the optimizer trace. Nothing is
timed, so it is cheap and deterministic. Doubles as the correctness gate:
every engine re-runs each query's `COUNT(*)` and must match the cached truth.

**`<ds>_read_perf`** (`perf_run.py`) — what those decisions *cost*. Fresh
server per cell, then 1 cold + 5 warm executions with engine counters. Cold
(nothing cached) is the headline axis; warm isolates plan CPU cost with I/O
removed. Gate: the full result set is fingerprinted and must be identical
across every engine and plan.

**`<ds>_write_perf`** (`ingest_run.py`) — the index-maintenance axis. Streams
the whole table as row-by-row INSERTs from 8 writer processes onto a throwaway
datadir, checkpointing every 10k rows for the slowdown curve. A layout's
maintenance cost is its run minus the same engine's `std` (PK-only) run, which
is why the two `std` identities are controls and must stay.

**`<ds>_read_plan_estimator_off`** (`run.py`) — ablation of the M5 estimator,
not a competing baseline. Same bitlsm cells as `read_plan` (which is the
estimator-on side: plan mode is deterministic, so no separate "on" set is
needed) with `--rocksdb-bitlsm-estimator=0`, a server flag and therefore not
expressible as a cell. Without engine statistics the optimizer falls back to
the server's own no-statistics guesses — 0.1 per equality and 0.33 per range,
i.e. `COND_FILTER_EQUALITY` / `COND_FILTER_INEQUALITY` in `sql/item.h`, the
System R heuristics — multiplied under independence. The point is not that a
constant loses; it is that no constant *can* win, since true selectivities
here span four to five orders of magnitude (SSB 8.3e-7…3.7e-2). Even a single
constant tuned after the fact to the geometric mean of the true values still
leaves a median q-error of 10.2 on SSB and 4.6 on taxpayer.

## Cells (identical for both datasets)

| cell | what it represents |
|---|---|
| `bitlsm/bi_v1` | bitmap index alone |
| `bitlsm/sk_bi_v1` | bitmap index added on top of conventional SKs (deployment-realistic); measured under `plan=auto`, so the optimizer picks between bi and the SKs on its own |
| `myrocks/sk_v1` | one SK per filter column, same LSM engine |
| `myrocks/composite_v1` | per-template optimal composites — the specialist upper bound, requires knowing the workload |
| `innodb/sk_v1` | one SK per filter column on the B-tree engine |
| `innodb/composite_v1` | specialist upper bound on the B-tree engine |

Histograms are built for every InnoDB/MyRocks cell at load time, so the SQL
engines always run with their full statistics toolbox.

Plans: every cell is measured under `auto` -- no hint at all, the optimizer
choosing unaided, which is what a deployment actually gets. The two composite
cells are the exception, pinned with `force_composite`. That layout represents
the per-template upper bound, and an upper bound only reached when the
statistics happen to cooperate is not one: unpinned, 6 of 21 taxpayer queries
run on the wrong composite, 2.6x slower than the layout can do. Pinning makes
the *baseline* faster, not BitLSM -- it is what puts the bound out of BitLSM's
reach.

A hinted plan therefore needs to earn its place: it must answer a question no
`auto` cell answers, and it must not simply override a choice the optimizer
should be making. `force_composite` is the only one that does.

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
