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
  "pk_mode": "uuid",                                    // "uuid" | "ulid"  (see below)
  "shuffle": false,                                     // seeded dataset shuffle (see below)
  "phases": [ { "label": "write", "type": "write_only" /* , "rows", "update_ratio" */ } ]
}
```

`pk_mode` and `shuffle` default to `"uuid"` / `false`, so configs that omit them
behave exactly as before.

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

## `ingestion` flags

Same `pk_mode` / `shuffle` are exposed as CLI flags:
`--pk_mode {uuid,ulid}` (default `uuid`), `--shuffle` (store-true).

## Example variants

| Config | `pk_mode` | `shuffle` | Regime |
|---|---|---|---|
| `workloads/write_seq_2024-2025_all.json` | uuid | false | non-time-correlated (zone maps weak) |
| `workloads/write_seq_2024-2025_all_ulid.json` | ulid | true | time-correlated (zone maps prune datetime ranges) |

Run both, build a DB from each, and compare embedded (zone map) vs BitLSM across
the two regimes. Read query workloads (`workloads/read_seq_*.json`) filter on
attributes only and are PK-agnostic, so the same query traces apply to both DBs.
