#!/usr/bin/env bash
# Build mysqld from the third_party/BitLSM-mysql-5.6 submodule.
#
# Usage: scripts/build_mysqld.sh <debug|release> [--reconfigure]
#
#   debug   -> build-mysql/debug    (CMAKE_BUILD_TYPE=Debug, plan-mode)
#   release -> build-mysql/release  (CMAKE_BUILD_TYPE=RelWithDebInfo, perf-mode)
#
# The sweep runners never build; humans run this script explicitly.
# Configure options mirror the original standalone checkout's CMakeCache
# (plus the MYSQL_MAINTAINER_MODE=OFF convention).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_DIR="$REPO_ROOT/third_party/BitLSM-mysql-5.6"
BOOST_DIR="${MYSQL_BOOST_DIR:-$HOME/Repositories/.mysql-boost}"

BUILD_KIND="${1:-}"
case "$BUILD_KIND" in
  debug)   CMAKE_BUILD_TYPE=Debug ;;
  release) CMAKE_BUILD_TYPE=RelWithDebInfo ;;
  *) echo "usage: $0 <debug|release> [--reconfigure]" >&2; exit 1 ;;
esac
BUILD_DIR="$REPO_ROOT/build-mysql/$BUILD_KIND"

if [[ ! -f "$SRC_DIR/CMakeLists.txt" ]]; then
  echo "error: submodule not checked out: $SRC_DIR" >&2
  echo "run: git submodule update --init third_party/BitLSM-mysql-5.6" >&2
  exit 1
fi
# mysql's cmake needs the RocksDB source tree nested two submodule levels deep:
# mysql -> rocksdb (BitLSM repo) -> third_party/rocksdb (facebook/rocksdb + patch).
if [[ ! -e "$SRC_DIR/rocksdb/third_party/rocksdb/src.mk" ]]; then
  echo "error: nested rocksdb submodule chain not fully checked out" >&2
  echo "run: git -C $SRC_DIR submodule update --init rocksdb" >&2
  echo "     git -C $SRC_DIR/rocksdb submodule update --init third_party/rocksdb" >&2
  exit 1
fi

mkdir -p "$BUILD_DIR"

MYSQL_COMMIT="$(git -C "$SRC_DIR" rev-parse HEAD)"
MYSQL_DATE="$(git -C "$SRC_DIR" log -1 --format=%cI)"
BITLSM_COMMIT="$(git -C "$SRC_DIR/rocksdb" rev-parse HEAD)"
BITLSM_DATE="$(git -C "$SRC_DIR/rocksdb" log -1 --format=%cI)"

# build.ninja only exists after a *successful* generate; CMakeCache.txt survives
# failed configures, so it is not a reliable "already configured" marker.
if [[ "${2:-}" == "--reconfigure" || ! -f "$BUILD_DIR/build.ninja" ]]; then
  # sql/CMakeLists.txt derives mysql_githash.h from $SRC_DIR/.git/logs/HEAD
  # unless MYSQL_GITHASH is set. As a submodule, $SRC_DIR/.git is a gitdir
  # pointer *file*, so that path can never exist and ninja refuses to build.
  # Passing all four values takes the documented opt-out. They are baked into
  # the cache, so a submodule bump needs --reconfigure to restamp.
  cmake -S "$SRC_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
    -DWITH_ROCKSDB=1 \
    -DWITH_BOOST="$BOOST_DIR" \
    -DDOWNLOAD_BOOST=1 \
    -DWITH_SSL=system \
    -DWITH_ZSTD=system \
    -DWITH_ZLIB=bundled \
    -DWITH_LZ4=bundled \
    -DWITH_UNIT_TESTS=OFF \
    -DMYSQL_MAINTAINER_MODE=OFF \
    -DMYSQL_GITHASH="$MYSQL_COMMIT" \
    -DMYSQL_GITDATE="$MYSQL_DATE" \
    -DROCKSDB_GITHASH="$BITLSM_COMMIT" \
    -DROCKSDB_GITDATE="$BITLSM_DATE"
fi

ninja -C "$BUILD_DIR" mysqld mysql

# Provenance stamp: consumed by the harness for per-run metadata (D1).
cat > "$BUILD_DIR/build-info.txt" <<EOF
build_kind=$BUILD_KIND
cmake_build_type=$CMAKE_BUILD_TYPE
mysql_commit=$MYSQL_COMMIT
bitlsm_commit=$BITLSM_COMMIT
built_at=$(date -Iseconds)
EOF

"$BUILD_DIR/runtime_output_directory/mysqld" --version
echo "OK: $BUILD_DIR"
