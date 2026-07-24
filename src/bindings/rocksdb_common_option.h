#pragma once
// Shared RocksDB I/O and memory regime, applied identically to every method so
// comparisons measure the index design rather than incidental option drift.
//
//   - Direct I/O for both user reads and flush/compaction removes the OS page
//     cache (an uncontrolled ~RAM-sized variable), leaving the block cache as
//     the sole data-residency knob. Per the RocksDB Direct-IO wiki, internal
//     auto-readahead compensates *scans* in direct mode; the record-CF path
//     here is point lookups (MultiGet), which readahead does not touch, so no
//     readahead tuning is needed.
//   - cache_index_and_filter_blocks makes index/filter blocks count against the
//     block-cache budget instead of living unbounded in the table reader, so a
//     stated "N MB cache" is honest for RocksDB-native blocks. The custom
//     SABI/SAI index blob is a separate heap allocation outside RocksDB's cache
//     (bounded by max_open_files, not by this budget) -- report RSS for the full
//     memory picture.
//   - One LRU cache is shared across every column family of a binding so the
//     budget is a single controlled quantity, sized by EXP_BLOCK_CACHE_MB.
//   - max_open_files is deliberately left at the RocksDB default (-1). Index
//     residency is a separate experimental axis and is not fixed here.
#include <cstdlib>
#include <memory>

#include <rocksdb/cache.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>

namespace experiment {

// One LRU cache per binding, shared across its column families. Sized from
// EXP_BLOCK_CACHE_MB (megabytes), default 1024.
inline std::shared_ptr<rocksdb::Cache> MakeExperimentBlockCache() {
  size_t mb = 1024;
  if (const char* e = std::getenv("EXP_BLOCK_CACHE_MB")) {
    long v = std::atol(e);
    if (v > 0) mb = static_cast<size_t>(v);
  }
  return rocksdb::NewLRUCache(mb << 20);
}

// DB-wide direct-I/O regime. On by default; EXP_DIRECT_IO=0 falls back to
// buffered I/O (an escape hatch, not for measurement runs).
inline void ApplyRocksdbCommonOptions(rocksdb::Options& opts) {
  bool direct = true;
  if (const char* e = std::getenv("EXP_DIRECT_IO"); e && e[0] == '0')
    direct = false;
  opts.use_direct_reads = direct;
  opts.use_direct_io_for_flush_and_compaction = direct;
  // compaction_readahead_size stays at its RocksDB default (2 MB).
}

// Memory-regime knobs on one table-options block. Pass the binding's shared
// cache so every column family draws from a single budget.
inline void ApplyRocksdbCommonTableOptions(
    rocksdb::BlockBasedTableOptions& topts,
    const std::shared_ptr<rocksdb::Cache>& cache) {
  topts.block_cache = cache;
  topts.cache_index_and_filter_blocks = true;
  // Pure LRU: no forced pinning and no hard capacity cap (a hard cap can fail
  // reads when a small cache is momentarily full). RSS is the memory metric.
  topts.pin_l0_filter_and_index_blocks_in_cache = false;
  topts.pin_top_level_index_and_filter = false;
}

}  // namespace experiment
