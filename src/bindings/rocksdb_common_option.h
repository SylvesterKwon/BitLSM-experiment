#pragma once
// Shared RocksDB I/O and table config, applied identically by every binding so
// comparisons measure the index design, not incidental option drift. Rationale
// for each choice lives in the commit that introduced this file.
#include <cstdlib>
#include <memory>

#include <rocksdb/cache.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/options.h>
#include <rocksdb/statistics.h>
#include <rocksdb/table.h>

namespace experiment {

// Block-cache counters for the memory-budget experiment: off unless
// EXP_CACHE_STATS is set, so ordinary measurement runs pay no ticker overhead.
// One process-wide Statistics instance shared by every column family (si-*
// opens two), so a driver can read one set of counters without reaching into
// the binding. Index-role tickers cover the UDI blob too: RocksDB classifies
// it as CacheEntryRole::kIndexBlock.
inline std::shared_ptr<rocksdb::Statistics> SharedCacheStatistics() {
  static std::shared_ptr<rocksdb::Statistics> stats =
      std::getenv("EXP_CACHE_STATS") ? rocksdb::CreateDBStatistics()
                                     : std::shared_ptr<rocksdb::Statistics>();
  return stats;
}

// Direct I/O for user reads and flush/compaction. On unless EXP_DIRECT_IO=0
// (buffered fallback, not for measurement runs). max_open_files and block_cache
// are left at their RocksDB defaults on purpose.
inline void ApplyRocksdbCommonOptions(rocksdb::Options& opts) {
  bool direct = true;
  if (const char* e = std::getenv("EXP_DIRECT_IO"); e && e[0] == '0')
    direct = false;
  opts.use_direct_reads = direct;
  opts.use_direct_io_for_flush_and_compaction = direct;
  // Write-amplification runs install their own Statistics before calling this;
  // never displace it.
  if (!opts.statistics) opts.statistics = SharedCacheStatistics();
}

// 4 KB data blocks: the controlled constant shared by every method (also the
// RocksDB default, restated so the shared config is self-describing).
//
// Memory-budget mode (the nyc_taxi_read_cache_budget read experiment): off
// unless EXP_BLOCK_CACHE_MB is set, so the standard config is byte-identical
// without it. When set, one process-wide LRU cache of that many MB caps total
// resident bytes. It is a function-local static so every column family's table
// factory shares the SAME instance -- this helper runs once per CF, and a
// method like si-* opens 2-3 factories, so a fresh cache per call would
// multiply the budget by the CF count. cache_index_and_filter_blocks pulls the
// index/filter/UDI blocks into that same cache, making the budget the TOTAL
// (data + index) rather than data-only. honk_player opens one DB per process,
// so process-wide sharing is exactly one DB's worth.
inline void ApplyRocksdbCommonTableOptions(
    rocksdb::BlockBasedTableOptions& topts) {
  topts.block_size = 4 * 1024;
  if (const char* mb = std::getenv("EXP_BLOCK_CACHE_MB")) {
    static std::shared_ptr<rocksdb::Cache> budget_cache = [&] {
      rocksdb::LRUCacheOptions copts;
      copts.capacity = static_cast<size_t>(std::atoll(mb)) << 20;
      // Pinned at 16 shards instead of the capacity-derived default (up to 64,
      // sharded_cache.cc GetDefaultCacheShardBits). A UDI blob is one cache
      // entry of several MB; at a small budget the default shard capacity
      // falls below a single blob, which would make the entry unplaceable in
      // its shard and concentrate eviction pressure there. 16 shards keeps
      // per-shard capacity well above one blob across this experiment's grid.
      copts.num_shard_bits = 4;
      return rocksdb::NewLRUCache(copts);
    }();
    topts.block_cache = budget_cache;
    topts.cache_index_and_filter_blocks = true;
  }
}

// Whole-key Bloom (10 bits/key) on the record CF. Separate from the helper above
// because secondary index CFs use a different filter or none, so this applies
// only to record-CF table options.
inline void ApplyRecordCfBloom(rocksdb::BlockBasedTableOptions& topts) {
  topts.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
}

}  // namespace experiment
