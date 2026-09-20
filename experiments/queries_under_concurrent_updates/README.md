# queries_under_concurrent_updates — query throughput under a concurrent update stream

Query throughput while one thread overwrites existing records at a fixed rate
W, in the style of Luo & Carey (VLDB 2020). Driver: `build/bin/qui_player`
(`src/honk_player/qui_player.cpp`); `run.py` sweeps W and ends with the tidy
CSVs (`summarize.py`, which also runs on its own); `plot_qps_vs_w.py` draws the
figure.

## Setup

| | |
|---|---|
| Data | NYC taxi 2024–2025, 89.9M rows, UUID keys, six indexed attributes |
| Methods | `bitlsm` (rho 0.001); `embedded-postings` with `--intersection_limit 2` (Top-2 Intersection) and `0` (Intersection). With two predicates both keep every predicate, so c = 2 runs one Embedded Postings arm |
| Queries | `read_seq_sel0.00001_k{2,3}_r300.tsv` (σ ∈ [1e-6, 1e-5)), one closed-loop worker, reshuffled each pass |
| Updates | one open-loop thread at W/s; each update overwrites a random existing key with another row's values. 20M-line sequence, replayed from the start when exhausted |
| W | 0, 1000, 4000, 16000, 32000, 64000, 128000 updates/s |
| Run | 30 min, 60 s windows, on a fresh copy of the base DB |
| Otherwise | as `nyc_taxi_seq_read`: 32 MB block cache, direct I/O, `--scan_prefetch_depth 32` |

## Steps

Base DBs and the update sequence are large and derived, so they are built here
rather than kept in the repo. The write trace the base DBs replay
(`workloads/write_seq_2024-2025_all_uuid.tsv`) is the one every NYC taxi
experiment uses; `src/honk/README.md` says how to make it.

```bash
# 1. Update sequence: 20M `u` lines sampled from the write trace (9.3 GB, seed 42, a few minutes)
PYTHONPATH=src python3 src/honk/honk_run.py overwrite \
    src/honk/workloads/overwrite_uniform_2024-2025_all_uuid_n20M.json --output_dir workloads

# 2. Base DBs -> /scratch/qui/uuid/{bitlsm/rho0.001, embedded-postings/default} (11 + 15 GB, ~8 min each)
#    Read fixtures: never --clean-db. Both Embedded Postings arms share one DB.
python3 experiments/nyc_taxi_seq_write/run.py experiments/queries_under_concurrent_updates/exp_set/write_seq_base_uuid.json \
    --pk_mode uuid --hw-reset --cooldown 60

# 3. The sweeps (c = 2: 14 runs, c = 3: 21 runs, ~32 min per run). Each one runs every W on a
#    fresh copy of the base DB and ends by writing its tidy CSVs.
python3 experiments/queries_under_concurrent_updates/run.py experiments/queries_under_concurrent_updates/exp_set/main_sel0.00001_k2.json --hw-reset --cooldown 60
python3 experiments/queries_under_concurrent_updates/run.py experiments/queries_under_concurrent_updates/exp_set/main_sel0.00001_k3.json --hw-reset --cooldown 60
```

`run.py` takes the common runner flags plus `--keep-run-db`; `--clean-db` is
ignored, and `--start-from` resumes into a new results directory. `--hw-reset`
needs passwordless sudo, and `/scratch` needs about 50 GB free for the base DBs
and the per-run copy.

Each run works on a copy of the base DB under `/scratch/qui/runs/`: SSTs
hard-linked, `CURRENT`/`MANIFEST`/`OPTIONS`/`IDENTITY`/WAL copied — what
`rocksdb::Checkpoint` does, without opening the base. After every run the
runner checks the base's files are unchanged and deletes the copy (it keeps
the copy of a failed run).

The paper figure comes from the two result directories:

```bash
python3 experiments/queries_under_concurrent_updates/plot_qps_vs_w.py results/<c2_dir> results/<c3_dir>
```

## Outputs

| File | One row per | Contents |
|---|---|---|
| `*_query_log.csv` | query | latency, records matched, whether it finished inside the window |
| `*_write_log.csv` | second | updates scheduled and issued, backlog, lag, Put latency |
| `*_lsm_log.csv` | second | memtable size, pending compaction, L0 files, write stalls |
| `*_meta.json` | run | parameters, totals, LSM state at start and end |
| `sweep_meta.json` | sweep | command, param set, input hashes, base DB inventories, commits |
| `queries_under_concurrent_updates_windows.csv` | method × W × window | QPS, median latency, backlog, stalls |
| `queries_under_concurrent_updates_windows_all.csv`, `queries_under_concurrent_updates_summary_all.csv` | as above, plus `c` | written when `summarize.py` is given several result directories |
| `queries_under_concurrent_updates_summary.csv` | method × W | QPS, no-load QPS and ratio, actual update rate, overloaded |
| `queries_under_concurrent_updates_qps_vs_w.pdf` | — | throughput against W, one panel per query set |

A run counts as overloaded when the writer falls behind: actual rate below
0.95·W, backlog at the end above W, or backlog rising over the last three
windows.

## Checks

| Command | Checks |
|---|---|
| `python3 -m unittest discover -s experiments/queries_under_concurrent_updates -p "test_*.py"` | DB copy, window/run statistics, overload rule, figure |
| `./build/bin/embedded_xcheck <four fresh paths>` | exact counts for every method with 10 % overwrites |
| `qui_player --verify --base_workload <trace>` | concurrent Put + Scan results bounded by the keys updated during each query |
| `./build/bin/lsm_stats_check <three paths>`, `./build/bin/qui_pacer_test` | LSM sampler, update pacer |
