# nyc_taxi_seq_read — NYC taxi read experiments

Replays read workloads (`workloads/read_seq/*.tsv`) with `honk_player` against
the DBs built by `nyc_taxi_seq_write`. DB paths are
`{db_path_base}/{pk_mode}/{method}/{params}`; every run writes one
`*_read_log.csv` per (workload, method) into `results/<timestamp>_<label>/`.

## Main sweep (UUID keys)

```bash
python3 experiments/nyc_taxi_seq_read/run.py experiments/nyc_taxi_seq_read/exp_set/read_seq_r300.json --pk_mode uuid --hw-reset --cooldown 10
python3 experiments/nyc_taxi_seq_read/summarize.py <result_dir>   # one tidy CSV
python3 experiments/nyc_taxi_seq_read/plot.py <result_dir>        # rows = c, columns = selectivity
```

Rho sensitivity: `exp_set/read_seq_rho_sensitivity.json` with
`plot_rho_sensitivity.py`.

## Key-correlated attributes (ULID keys)

The primary key is a ULID timestamped with `tpep_pickup_datetime`, so both
datetime attributes follow key order. Methods: `bitlsm`, `bitlsm-global`
(BitLSM with equi-depth bin boundaries computed once over the whole dataset,
same rho; `src/bindings/global_bins/`) and `embedded` (per-block zone maps and
Bloom filters). Queries are time windows, `pickup >= x AND dropoff < y`, at
ten selectivity levels `10^(-5+k/3)`, k = 0..9, 100 queries per level, each
within 2 % of its level (`src/honk/interval_window.py`).

```bash
# 1. Release build and the fork gate (must print "all checks passed")
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && ninja -C build
./build/bin/global_bins_xcheck

# 2. DBs (read fixtures: no --clean-db). bitlsm-global reads its oracle bin
#    policy from experiments/nyc_taxi_seq_write/bin_policy/ (committed; a
#    deterministic function of the write workload and rho, regenerated with
#    build/bin/global_bin_policy --workload workloads/write_seq_2024-2025_all_ulid.tsv
#    --indexed_attrs PULocationID,DOLocationID,tpep_pickup_datetime,tpep_dropoff_datetime,fare_amount,passenger_count
#    --rho 0.001 --output <file>)
python3 experiments/nyc_taxi_seq_write/run.py experiments/nyc_taxi_seq_write/exp_set/write_seq_ulid_key_correlation.json \
    --pk_mode ulid --hw-reset --cooldown 60

# 3. Window workloads (committed under workloads/read_seq; regenerate with)
for tag in 1.00e-05 2.15e-05 4.64e-05 1.00e-04 2.15e-04 4.64e-04 1.00e-03 2.15e-03 4.64e-03 1.00e-02; do
    PYTHONPATH=src python3 src/honk/honk_run.py run src/honk/workloads/read_window_sel${tag}_r100.json \
        --output_dir workloads/read_seq --data_dir data
done

# 4. Reads
python3 experiments/nyc_taxi_seq_read/run.py experiments/nyc_taxi_seq_read/exp_set/read_window_ulid_key_correlation.json \
    --pk_mode ulid --hw-reset --cooldown 10

# 5. Candidate rows per query (offline, untimed; the two BitLSM arms)
python3 experiments/nyc_taxi_seq_read/count_candidates.py experiments/nyc_taxi_seq_read/exp_set/read_window_ulid_key_correlation.json \
    --pk_mode ulid

# 6. Summary and figure
python3 experiments/nyc_taxi_seq_read/summarize_key_correlation.py <read_result_dir> <candidates_result_dir>
python3 experiments/nyc_taxi_seq_read/plot_key_correlation.py <read_result_dir>
```

Outputs in `<read_result_dir>`:

- `key_correlation_queries.csv` — one row per (workload, method, query):
  latency, records matched, data read, and for the BitLSM arms the SABI
  candidates and candidate blocks.
- `key_correlation_summary.csv` — one row per (level, method): median and
  mean latency, mean matched, mean candidates, mean candidate blocks.
- `key_correlation_latency.pdf` — mean latency per level, single column.

`bitlsm-global` DBs are built with `--bin_policy <file>` (the write param set
passes the committed file) and queried without it. Query mode aborts if the live SST set
differs from the one recorded at build time, since a flush or compaction under
unmodified BitLSM would rewrite SSTs with local bins.

`exp_set/read_seq_ulid_key_correlation.json` runs the main sweep's
`sel0.0001` workloads on the same ULID DBs; `summarize_key_correlation.py`
splits its queries by whether a predicate falls on a datetime attribute.
