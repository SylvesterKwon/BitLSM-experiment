# BitLSM-experiment

## Installation

GCC 11+, CMake 3.14+.

### 1. Submodules

```bash
git submodule update --init --recursive third_party/BitLSM
git submodule update --init third_party/BitLSM-mysql-5.6
git -C third_party/BitLSM-mysql-5.6 submodule update --init rocksdb
git -C third_party/BitLSM-mysql-5.6/rocksdb submodule update --init third_party/rocksdb
git submodule update --init third_party/ssb-dbgen
```

Not a plain `--recursive` at the root: that also clones MySQL's vendored boost
(615MB, unused — step 4 points `WITH_BOOST` elsewhere) and faiss.

### 2. System dependencies

```bash
sudo apt-get update && sudo apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git ca-certificates \
    libsnappy-dev liblz4-dev libzstd-dev libjemalloc-dev \
    libgflags-dev zlib1g-dev libbz2-dev \
    bison libncurses-dev libssl-dev
```

### 3. Build the experiment binaries

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build -j$(nproc)
```

### 4. Build mysqld (MyRocks / InnoDB baselines)

Boost 1.77.0 must be placed by hand — MySQL 8.0.32 downloads it from
`boostorg.jfrog.io`, which is shut down:

```bash
mkdir -p ~/Repositories/.mysql-boost && cd ~/Repositories/.mysql-boost
curl -fLO https://archives.boost.io/release/1.77.0/source/boost_1_77_0.tar.bz2
tar xjf boost_1_77_0.tar.bz2
```

Set `MYSQL_BOOST_DIR` if you keep boost elsewhere. Then:

```bash
scripts/build_mysqld.sh release
```

### 5. Python client (optional, for MySQL experiments)

Not needed to build anything — only to run the MySQL experiment runners:

```bash
pip install mysql-connector-python
```
