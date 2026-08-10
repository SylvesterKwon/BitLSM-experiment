# BitLSM-experiment

## Installation

Verified on a clean Ubuntu 22.04 host (gcc 11.4, cmake 3.22.1, ninja 1.10.1).

### Prerequisites

- **OS**: Linux. Ubuntu 22.04 is the reference platform (matches the BitLSM CI image).
- **Compiler**: GCC 11+ (C++20).
- **CMake**: 3.14+, plus Ninja.
- **Network**: the configure step fetches cxxopts, nlohmann/json, simdjson and
  CRoaring via `FetchContent`, so the first `cmake -B build` needs internet access.

### 1. Submodules

The core engine lives in `third_party/BitLSM`, which itself vendors a patched
facebook/rocksdb as a nested submodule — `--recursive` is required:

```bash
git submodule update --init --recursive third_party/BitLSM
```

The BitLSM patch (`third_party/BitLSM/patches/rocksdb-bitlsm.patch`) is applied to
the RocksDB tree automatically at configure time; re-running configure on an
already-patched tree is a no-op, so no manual patching step is needed.

The other two submodules are **not** needed for this build:
`third_party/BitLSM-mysql-5.6` and `third_party/ssb-dbgen` are only used by the
`myrocks_integration_test` experiment (see below).

### 2. System dependencies

```bash
sudo apt-get update && sudo apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git ca-certificates \
    libsnappy-dev liblz4-dev libzstd-dev libjemalloc-dev \
    libgflags-dev zlib1g-dev libbz2-dev
```

The last three go beyond what the BitLSM submodule's own README lists: this repo's
root `CMakeLists.txt` additionally calls `find_package(gflags REQUIRED)` and links
`z` and `bz2` in `COMMON_LINK_LIBS`, so building BitLSM standalone succeeds without
them while this repo fails to configure.

### 3. Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build -j$(nproc)
```

`Release` compiles with `-O3 -march=native -mtune=native`, so the resulting binaries
are tuned to the build host and should not be copied to a different CPU. Other build
types are `Debug` (`-O0 -g`) and `RelWithDebInfo`. A cold build is dominated by
RocksDB (~450 targets, roughly 10 minutes on 24 cores).

Artifacts land in `build/bin/`:

| Binary | Source | Purpose |
|---|---|---|
| `bit-lsm`, `embedded`, `sai`, `no-index`, `si-ck`, `si-lu`, `si-eager` | `src/benchmark/methods/` | Synthetic benchmark driver, one per method |
| `honk_player` | `src/honk_player/` | Real-world (NYC taxi TSV trace) driver |
| `embedded_xcheck` | `src/benchmark/` | Embedded-index cross-check tool |
| `sai_test_*` | `src/bindings/sai/test/` | SAI unit tests (assert-based standalone mains) |

### 4. Verify the build

Run the SAI unit tests — each exits non-zero on failure:

```bash
for t in build/bin/sai_test_*; do "$t" >/dev/null && echo "PASS $t" || echo "FAIL $t"; done
```

Then a quick end-to-end write + read against a throwaway DB:

```bash
./build/bin/bit-lsm --exp_type write_seq -n 200000 \
    --schema src/benchmark/schema/default_a4_c100.json \
    -d /tmp/bitlsm_smoke/db -o /tmp/bitlsm_smoke/out --exp_label smoke

./build/bin/bit-lsm --exp_type read_seq -n 200000 \
    --schema src/benchmark/schema/default_a4_c100.json \
    -d /tmp/bitlsm_smoke/db -o /tmp/bitlsm_smoke/out --exp_label smoke \
    --query_attr_indices 0 --selectivity 0.05
```

The read prints `RESULT:{elapsed_ms},{matched},{selectivity}`. Swapping `bit-lsm`
for `no-index` on the same schema and record count must yield the same `matched`
count. Note that `-n` is required for `read_seq` as well as `write_seq`.

### 5. Python runners (optional)

The sweep runners under `experiments/` and `src/run.py` need Python 3.10+ and, for
plotting and workload generation:

```bash
pip install matplotlib pandas numpy        # runners + plot.py
pip install -r src/honk/requirements.txt   # NYC taxi workload generation
```

`--hw-reset` in any runner shells out to `sudo` for `drop_caches` and `fstrim`; omit
the flag if the host has no passwordless sudo.

### 6. MyRocks integration build (optional)

The `myrocks_integration_test` experiment builds a separate MySQL 5.6 fork into
`build-mysql/` and is independent of the CMake build above. It has its own
prerequisites (bison, ncurses, a hand-placed Boost 1.77.0, two more submodule
levels) — see [`experiments/myrocks_integration_test/README.md`](experiments/myrocks_integration_test/README.md).
