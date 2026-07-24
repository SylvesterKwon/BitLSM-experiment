#pragma once
// Shared RocksDB I/O and memory regime, applied identically by every binding so
// comparisons measure the index design, not incidental option drift. Rationale
// for each choice lives in the commit that introduced this file.
#include <cstdlib>
#include <memory>

#include <rocksdb/cache.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>

namespace experiment {

// One LRU shared across a binding's column families, sized by EXP_BLOCK_CACHE_MB
// (megabytes, default 1024) so the data-block budget is a single quantity.
inline std::shared_ptr<rocksdb::Cache> MakeExperimentBlockCache() {
  size_t mb = 1024;
  if (const char* e = std::getenv("EXP_BLOCK_CACHE_MB")) {
    long v = std::atol(e);
    if (v > 0) mb = static_cast<size_t>(v);
  }
  return rocksdb::NewLRUCache(mb << 20);
}

// Direct I/O for user reads and flush/compaction. On unless EXP_DIRECT_IO=0
// (buffered fallback, not for measurement runs). max_open_files is left at the
// RocksDB default (-1) on purpose: index residency is a separate axis.
inline void ApplyRocksdbCommonOptions(rocksdb::Options& opts) {
  bool direct = true;
  if (const char* e = std::getenv("EXP_DIRECT_IO"); e && e[0] == '0')
    direct = false;
  opts.use_direct_reads = direct;
  opts.use_direct_io_for_flush_and_compaction = direct;
}

// Regime for one table-options block; pass the binding's shared cache so every
// CF draws from one data-block budget. cache_index_and_filter_blocks is left at
// its default (false): index/filter blocks are pre-loaded and pinned in the
// table reader, so the record-CF Bloom never gets evicted (stable MultiGet) and
// the cache is purely a data budget. The custom SABI/SAI index is resident and,
// like the native index/filter, outside the cache -- RSS is the memory metric.
inline void ApplyRocksdbCommonTableOptions(
    rocksdb::BlockBasedTableOptions& topts,
    const std::shared_ptr<rocksdb::Cache>& cache) {
  topts.block_size = 4 * 1024;
  topts.block_cache = cache;
}

// Whole-key Bloom (10 bits/key) on the record CF. Separate from the helper above
// because secondary index CFs use a different filter or none, so this applies
// only to record-CF table options.
inline void ApplyRecordCfBloom(rocksdb::BlockBasedTableOptions& topts) {
  topts.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
}

}  // namespace experiment
