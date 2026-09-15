# Experiment Conventions

## Architecture

All experiment methods (bitlsm, no-index, si-ck, si-lu, si-eager) share a **Binding** virtual interface (`bindings/binding.h`). Each binding implements `Open`, `Put`, `Scan`, `Close`. Two drivers consume bindings:

1. **BenchmarkExperiment** (`benchmark/benchmark_experiment.h`) — synthetic workload driver. Generates random KVPs from a schema, runs write/read experiments.
2. **HonkPlayer** (`honk_player/honk_player.cpp`) — real-world workload driver. Replays taxi data TSV traces.

Both drivers are method-agnostic; the binding is selected at runtime via `CreateBinding(name)`.

## Directory Layout

```
results/                       # all experiment outputs (auto-created)
│   └── YYYYMMDD_HHMM_<label>/ # timestamped result directories
experiments/                   # standalone experiments (each has its own runner + spec)
│   └── <experiment_name>/     # e.g. seq_write, nyc_taxi_seq_read
│       ├── exp_set/           # param set JSON files
│       └── run.py             # experiment runner
src/
├── run_common.py              # shared runner utilities (logging, hw-reset, daemon, etc.)
├── run.py                     # synthetic benchmark sweep runner
├── bindings/                  # Binding interface + implementations (static library)
│   ├── binding.h              # virtual interface: Open/Put/Scan/Close
│   ├── binding_factory.cpp    # CreateBinding() factory
│   ├── bitlsm_binding.*      # BitLSM binding
│   ├── no_index_binding.*     # No-Index (plain RocksDB) binding
│   ├── si_ck_binding.*        # SI-CK (concatenated key) binding
│   ├── si_lu_binding.*        # SI-LU (list union + merge operator) binding
│   └── si_eager_binding.*     # SI-Eager (eager sorted insert) binding
├── benchmark/
│   ├── benchmark_experiment.h # BenchmarkExperiment class (uses Binding)
│   ├── si_benchmark_common.h  # shared SI utilities (TransactionDB, scan helpers)
│   ├── methods/               # one-liner main() per method → build/bin/<method>
│   ├── schema/                # schema JSON files
│   ├── exp_set/               # sweep parameter JSON files
│   └── result/                # CSV outputs (auto-created)
└── honk_player/
    ├── honk_player.cpp        # real-world workload driver (→ build/bin/honk_player)
    ├── honk_run.py            # honk_player sweep runner
    ├── taxi_schema.h          # NYC taxi column definitions
    ├── json_record_parser.h   # JSON → Attr/Query conversion
    ├── tsv_parser.h           # TSV workload reader
    ├── exp_set/               # honk sweep parameter JSON files
    ├── test/                  # test workload + generator
    └── result/                # CSV outputs (auto-created)
```

## Methods

| Method | Binding | Binary | Description |
|---|---|---|---|
| bitlsm | `BitLSMBinding` | `build/bin/bit-lsm` | Bitmap-indexed LSM-Tree |
| bitlsm-global | `BitLSMGlobalBinding` | `honk_player` only | Ablation: BitLSM with oracle global bin boundaries (`src/bindings/global_bins/`, forked SABI builder; core untouched). Build with `--bin_policy <file>` from `build/bin/global_bin_policy`, query without it. Gate: `build/bin/global_bins_xcheck` |
| no-index | `NoIndexBinding` | `build/bin/no-index` | Plain RocksDB (full table scan) |
| si-ck | `SICKBinding` | `build/bin/si-ck` | Secondary index — concatenated key |
| si-lu | `SILUBinding` | `build/bin/si-lu` | Secondary index — list union (merge operator) |
| si-eager | `SIEagerBinding` | `build/bin/si-eager` | Secondary index — eager sorted insert |

Each `methods/<method>.cpp` is a one-liner main:
```cpp
#include "benchmark_experiment.h"
#include "binding.h"
int main(int argc, char* argv[]) {
  return benchmark::BenchmarkExperiment(experiment::CreateBinding("bitlsm"))
      .Run(argc, argv);
}
```

## CLI Flags

### Benchmark binaries (common)

`--exp_label`, `--exp_type` (`write_seq`|`read_seq`), `-n`, `--schema`, `-d/--db_path`, `-o/--output_dir`

### Read-specific

`--query_attr_indices` (comma-sep), `--selectivity` (double)

### Method-specific

- `--rho` (double, bitlsm only)
- `--read_strategy` (`im`|`pf`, si-ck/si-lu/si-eager only)

### HonkPlayer

`--binding`, `--workload`, `--db_path`, `--output_dir`, plus method-specific flags passed through.

## Sweep Runners

### run.py (synthetic benchmark)

```bash
python3 src/run.py <params.json>                  # run all (daemon by default)
python3 src/run.py <params.json> --no-daemon      # foreground
python3 src/run.py <params.json> --dry-run        # print only
python3 src/run.py <params.json> --methods no-index,bitlsm
python3 src/run.py <params.json> --hw-reset --cooldown 300
python3 src/run.py <params.json> --start-from 5
```

### honk_run.py (real-world workload)

```bash
python3 src/honk_player/honk_run.py <params.json>            # daemon by default
python3 src/honk_player/honk_run.py <params.json> --no-daemon
python3 src/honk_player/honk_run.py <params.json> --dry-run
```

### Common runner options

| Flag | Default | Description |
|---|---|---|
| `--dry-run` | off | Print commands without executing |
| `--methods` | all | Comma-separated method filter |
| `--cooldown N` | 0 | Seconds between runs |
| `--hw-reset` | off | sync + drop_caches + fstrim between runs (sudo) |
| `--clean-db` | off | Delete DB directories before each write run |
| `--start-from N` | 1 | Resume from N-th experiment (1-indexed) |
| `--daemon` | **on** | Run in background via nohup |
| `--no-daemon` | off | Run in foreground |

Between-run sequence: `[--clean-db: rm DB] → experiment → [hw-reset: sync, drop_caches, fstrim] → [cooldown] → next`

Logs are saved to `logs/{timestamp}_{label}.log`.

## Performance: Hot-Path Allocation Rules

`Put()` is called up to 1e8 times per run. Heap allocation inside this loop directly impacts throughput.

**Rules for binding implementations:**
- `Put()` must not allocate heap memory per call. Buffers like `serialized_value_` must be class member variables, reused via `resize()`/`clear()`.
- `EncodeValue()` uses `out_value.resize(exact_size)` + `memcpy` — safe to reuse the same `string&`.

**Rules enforced in `BenchmarkExperiment`:**
- `vector<Attr>`, `payload`, `pk` are allocated once before the loop and reused per iteration.

When adding a new binding, follow the existing pattern in `no_index_binding.cpp` and `bitlsm_binding.cpp`.

## Result Files

All results are written to `results/YYYYMMDD_HHMM_<exp_label>/` under the project root. The timestamp is minute-granularity, generated once per runner invocation via `make_result_dir()` in `run_common.py`.

- Write CSV: `time_elapsed_ms,records_written` (checkpoint every 1M records)
- Read CSV (benchmark): `RESULT:{elapsed_ms},{matched},{selectivity}` printed to stdout, collected by run.py into master CSV
- Read CSV (honk): `query_id,query_attr_num,filter_attrs,time_elapsed_ms,records_matched,records_total,selectivity_actual`

## Standalone Experiments (`experiments/`)

Each experiment lives in its own directory under `experiments/`. Every experiment directory contains:

- `exp_set/` — param set JSON files (one per sweep configuration)
- `run.py` — self-contained runner with its own logic. Imports `src/run_common.py` for shared utilities (logging, daemon, hw-reset, etc.) but owns the experiment flow.
- `summarize.py` — optional; reduces one result directory to a single tidy CSV (`seq_write`, `nyc_taxi_seq_read`)
- `result/` — auto-created output directory for CSVs and plots

Each experiment runner defines its own logic rather than branching on `exp_type`. Param set JSONs only carry data (methods, parameters, paths), not behavior.

Runners accept the same common flags (`--dry-run`, `--methods`, `--cooldown`, `--hw-reset`, `--clean-db`, `--start-from`, `--daemon`/`--no-daemon`) and take a param set JSON as positional arg.

```bash
python3 experiments/<name>/run.py experiments/<name>/exp_set/<params>.json [options]
```

### Experiments

| Experiment | Description | Binary |
|---|---|---|
| `myrocks_integration_test` | SQL-level BitLSM×MyRocks vs InnoDB/MyRocks (plan/perf/ingest; SSB-flat + Public BI Taxpayer) | `build-mysql/release/bin/mysqld` |
| `seq_write` | Synthetic sequential write — time + DB size | benchmark binaries |
| `nyc_taxi_seq_write` | NYC taxi sequential write | `honk_player` |
| `nyc_taxi_seq_read` | NYC taxi sequential read (DB must exist) | `honk_player` |
| `nyc_taxi_interleave` | NYC taxi read-under-ingestion | `honk_player` |
| `memory_pressure` | NYC taxi read under a block-cache budget (DB must exist) | `honk_player` |

## Plotting

- Plotting scripts live under each experiment directory (e.g. `experiments/<name>/plot.py`).
- Generated plots (PNG, PDF, etc.) are saved into the same `result/` subdirectory that contains the source CSV files, keeping data and visualizations co-located.

### Figure style

Figures are sized and styled for a two-column paper, not for a screen. Match
these across experiments:

- `plt.rcParams.update({"font.size": 6})`.
- Width 7 in for a figure spanning both columns, 3.333 in for a single-column
  one.
- One panel's plot box is 0.702 as tall as it is wide -- measured from
  `nyc_taxi_seq_read`'s 4x4 grid, whose panels come out 1.294 x 0.909 in --
  so figures share a shape across the paper. A grid reaches it on its own
  (`figsize=(7, 7 * 3/4)`); a single row of panels does not, because the
  legend and axis labels take a far larger share of a short figure, so set
  `ax.set_box_aspect(0.702)` there. Do not eyeball the number: build the
  reference figure and read `ax.get_position()`. With the aspect pinned the
  box size no longer follows figsize -- extra height becomes margin -- so
  choose the height that leaves the legend clear of the panel titles.
- Axis values are written out (128, 256, ... 4096), not as `2^n`, and in one
  unit throughout: MB stays MB, never switching to GB partway along the axis.
  Where the panel is too narrow for every value, draw all the ticks but label
  every other one -- the measurement points stay visible and the labels stop
  colliding.
- A row of panels carries one shared `fig.supxlabel`, not the same label four
  times.
- Panels in one figure share a y axis (`sharey=True`) so values can be
  compared across panels, and inner panels drop their duplicate tick labels.
- Every y axis in a budget figure is log, matching `nyc_taxi_seq_read`, and
  mixing scales across figures of one experiment is worse than any per-figure
  gain. A shared linear axis flattens the small-c panels into the bottom
  quarter -- a 2.4x gap there reads as no gap -- while on a log axis the
  vertical distance between two lines is their ratio, which is what these
  figures are about. With the budget axis already log, a log y also
  straightens the `y = budget` reference line in the RSS figure.
- No gridlines: `ax.grid(False)`.
- Ticks point inward, short and hairline-thin: `direction="in"`, major size 2,
  width 0.3, minor size 1. Set it once through `rcParams` when a script draws
  several figures, or per-axis as in `seq_write_wa/plot.py`.
- Legends carry `frameon=False`; save with `dpi=150`.
- Latency is the MEDIAN of the per-query rows, matching the boxplot centre
  lines in `nyc_taxi_seq_read`, so a configuration measured in both sections
  reads the same in both. Byte volumes are averages (see below): they are a
  different kind of quantity, so the mix is deliberate -- label each axis with
  its statistic, and have any summary CSV carry both.
- Figures meant to be stacked in the paper draw each piece of shared furniture
  once: column titles and the legend on the top row, the x label on the bottom.
  Pin the panel geometry with `subplots_adjust` rather than `tight_layout` --
  y labels differ in length, so a per-figure layout would misalign the columns.
  Size each row to its box plus only the furniture it carries: an axes area
  taller than the box just pads around it (the gap that shows under a stacked
  row), and one shorter shrinks the box, which narrows it too and breaks the
  alignment. A rotated y label is longer than the box, so leave it room.
- One word per statistic across a project: this one says "mean", never "avg"
  or "average", in axis labels, CSV column names and prose alike.
- Axis labels name their statistic -- `Median query latency (s)`,
  `Mean index read (MB/query)` -- so a reader never has to guess which
  one a figure used.
- Byte volumes are reported PER QUERY (run total / query count), not as run
  totals and not as medians. They are a cost that accumulates, so the middle
  query does not represent them, and the per-query volume falls across a run
  as the cache warms. Per-query also keeps the axis in MB, where "this query
  touched 380 MB of index" reads as a property of the workload; a 120 GB run
  total reads as a mistake. The two statistics differ on purpose: latency is
  what one query costs the user, bytes are what the workload costs the
  device.
- Legend entries name the method, not the run configuration, and never carry
  a flag name the reader would have to look up. Drop a mode suffix when every
  series shares that mode. Label BitLSM as `BitLSM` without its rho -- rho
  belongs in the legend only where it is the swept variable
  (`plot_rho_sensitivity.py`).
- When a baseline appears in two configurations, label both symmetrically by
  what differs, so the reader sees the single axis of variation at a glance:
  `SAI (intersect top-2)` and `SAI (intersect all)`, never `il=0` / `il=2`.
  Keep the verb -- bare `SAI (2 predicates)` reads as a query shape next to
  panels titled `c = 2`. The shipped default (Cassandra intersects its two
  most selective predicates) is solid and listed first; the variant we added
  to be generous to the baseline is dashed.
- The `embedded` baseline is named Per-Block Filters. Naming its two
  structures instead (`Bloom + Zone Map`) reads as two series in a legend that
  draws one box for it, and its `bloom_bits` is a build parameter the figure
  never varies, so it stays out of the label the same way BitLSM's rho does.
- BitLSM-Global (the `bitlsm-global` ablation arm) is orange `#E69F00`, solid,
  with no hatch or dash. BitLSM's own red at the size of a single-column box
  swallows the usual same-hue hatch, so the ablation arm gets its own color
  instead. The methods read Per-Block Filters, BitLSM-Global, BitLSM, in that
  order.
- Do not hedge a re-implemented baseline in the legend (`SAI-like`). The text
  says once that it is a best-effort implementation; repeating it per figure
  invites the "how unlike?" question that the paper deliberately does not
  spend space answering.
