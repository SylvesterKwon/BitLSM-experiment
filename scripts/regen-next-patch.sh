#!/usr/bin/env bash
# Regenerate patches/next-multiattr.patch from the current NEXT rocksdb-7.7.3
# submodule tree.
#
# Workflow for editing NEXT's RocksDB internals:
#   1. Edit files under third_party/NEXT/rocksdb-7.7.3/ directly (the patch is
#      already applied to the working tree after scripts/build_next.sh runs).
#   2. Run this script to capture the working-tree changes back into the patch
#      file.
#   3. Commit the updated patches/next-multiattr.patch.
#
# To discard local NEXT RocksDB edits and return to the pinned vanilla tree:
#   git -C third_party/NEXT/rocksdb-7.7.3 checkout .
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
rocksdb_src="${repo_root}/third_party/NEXT/rocksdb-7.7.3"
patch_file="${repo_root}/patches/next-multiattr.patch"

[[ -d "${rocksdb_src}" ]] || { echo "error: ${rocksdb_src} not found — run: git submodule update --init third_party/NEXT" >&2; exit 1; }

git -C "${rocksdb_src}" diff --full-index > "${patch_file}"
echo "Regenerated ${patch_file} ($(wc -l < "${patch_file}") lines)"
