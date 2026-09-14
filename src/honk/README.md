# Honk Workload Generator

Generates TSV workload traces (write / update / read ops) from NYC Yellow Taxi
parquet data, for replay by `honk_player` (the C++ real-world driver).

## Running

```bash
# Config-driven generation
PYTHONPATH=src python3 src/honk/honk_run.py run <config.json> \
    --output_dir <out_dir> --data_dir <parquet_dir>

# Query-under-ingestion generation (6.2.2)
PYTHONPATH=src python3 src/honk/honk_run.py ingestion \
    --output_dir <out_dir> --data_dir <parquet_dir> [options]
```

Download the source parquet first: `python3 src/honk/download.py --year 2024-2025 -y`.

## Workload config schema (`run`)

```jsonc
{
  "dataset": ["yellow_tripdata_2024-01.parquet", ...],  // consumed in this order
  "seed": 42,                                           // RNG seed (reproducible)
  "pk_mode": "ulid",                                    // "uuid" | "ulid" — REQUIRED (no default)
  "shuffle": true,                                      // seeded dataset shuffle (see below)
  "phases": [ { "label": "write", "type": "write_only" /* , "rows", "update_ratio" */ } ]
}
```

`pk_mode` is **required** — there is no default, because the two regimes build
different DBs and the choice should be explicit. `shuffle` defaults to `false`.

### `pk_mode` — primary-key scheme (controls zone-map / time-correlation regime)

The LSM stores data sorted by primary key, so a data block holds a contiguous
PK range. Whether an **embedded zone map** can prune a continuous-attribute
range query depends on whether that attribute is correlated with the PK.

| `pk_mode` | PK | Effect |
|---|---|---|
| `"uuid"` (default) | random UUIDv4 | PK uncorrelated → every block is a random sample → zone maps **cannot** prune any continuous attribute (non-time-correlated regime). |
| `"ulid"` | ULID whose 48-bit timestamp is `tpep_pickup_datetime` (80-bit randomness from the seeded RNG) | base32 string sorts chronologically → after compaction, blocks are time-contiguous → zone maps **prune** pickup/dropoff range queries (time-correlated regime, à la Qader's `tweet_id`). |

ULID only makes `tpep_pickup_datetime` (and, by derivation, `tpep_dropoff_datetime`)
time-correlated; other continuous attributes (`fare_amount`, `trip_distance`, …)
stay uncorrelated.

### `shuffle` — decouple insertion order from the PK

Seeded shuffle of the dataset before consuming, so write order is random
regardless of `pk_mode`. Recommended `true` for the `ulid` variant: it keeps the
write/compaction pattern a realistic random-write stress (matching the uuid
variant) so the **only** difference between the two variants is PK↔attribute
correlation. The final on-disk DB is PK-sorted either way, so zone maps still
work. (Note: `sample(frac=1)` copies the frame, ~2× peak memory.)

### `interval_window` — fixed two-predicate window queries

`start_attr >= x AND end_attr < y`: the intervals that begin and end inside one
window (trips picked up after x and dropped off before y). The attribute pair
is fixed by the block, so only the window moves. A target selectivity is drawn
from the band, x is a random row's start value and y is the k-th smallest end
value among the rows starting at or after x, so the query matches exactly k
rows; a tight band (e.g. ±2 %) pins every query to one selectivity level.
Used by the ULID key-correlation experiment, where selectivity against the
global bin's share of rows is the variable under test.

```jsonc
{ "label": "window_sel0.0001", "strategy": "interval_window",
  "query_attr_num": 2,                                   // always 2
  "window": { "start_attr": "tpep_pickup_datetime",
              "end_attr": "tpep_dropoff_datetime" },
  "target_selectivity_percent": { "lo": 0.001, "hi": 0.01 } }
```

The open side of each bound is written as a sentinel (1970 / 2100) inside an
ordinary two-sided `range`, so the player needs no one-sided range support and
BitLSM folds each pair into one interval. Configs: `workloads/read_point_sel*_r100.json`
(ten levels, 10^(-5+k/3), 100 queries each) and the earlier band-wide
`workloads/read_window_sel*_r300.json`.

## `ingestion` flags

Same `pk_mode` / `shuffle` are exposed as CLI flags:
`--pk_mode {uuid,ulid}` (default `uuid`), `--shuffle` (store-true).

## Example variants

| Config | `pk_mode` | `shuffle` | Regime |
|---|---|---|---|
| `workloads/write_seq_2024-2025_all_ulid.json` | ulid | true | time-correlated (zone maps prune datetime ranges) |
| `workloads/write_seq_2024-2025_all_uuid.json` | uuid | true | non-time-correlated (random PK → zone maps cannot prune) |

Each config generates `write_seq_2024-2025_all_<pk_mode>.tsv`.

**Experiment runners are `--pk_mode`-aware (and require it).** The taxi runners
(`nyc_taxi_seq_write`, `nyc_taxi_seq_read`, `nyc_taxi_interleave`) take a
**required** `--pk_mode {uuid,ulid}` flag that namespaces the DB path as
`{db_path_base}/{pk_mode}/{method}/{params}`, so the two regimes' DBs never mix.
Example: build ULID DBs, then read them:

```bash
python3 experiments/nyc_taxi_seq_write/run.py .../exp_set/write_seq_all_ulid.json --pk_mode ulid
python3 experiments/nyc_taxi_seq_read/run.py  .../exp_set/read_seq_sel0.01_r300.json --pk_mode ulid
```

Run both regimes and compare embedded (zone map) vs BitLSM across them. Read query workloads (`workloads/read_seq_*.json`) filter on
attributes only and are PK-agnostic, so the same query traces apply to both DBs.
