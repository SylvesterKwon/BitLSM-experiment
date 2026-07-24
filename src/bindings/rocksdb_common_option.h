#pragma once
// Shared RocksDB I/O and table config, applied identically by every binding so
// comparisons measure the index design, not incidental option drift. Rationale
// for each choice lives in the commit that introduced this file.
#include <cstdlib>

#include <rocksdb/filter_policy.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>

namespace experiment {

// Direct I/O for user reads and flush/compaction. On unless EXP_DIRECT_IO=0
// (buffered fallback, not for measurement runs). max_open_files and block_cache
// are left at their RocksDB defaults on purpose.
inline void ApplyRocksdbCommonOptions(rocksdb::Options& opts) {
  bool direct = true;
  if (const char* e = std::getenv("EXP_DIRECT_IO"); e && e[0] == '0')
    direct = false;
  opts.use_direct_reads = direct;
  opts.use_direct_io_for_flush_and_compaction = direct;
}

// 4 KB data blocks: the controlled constant shared by every method (also the
// RocksDB default, restated so the shared config is self-describing).
inline void ApplyRocksdbCommonTableOptions(
    rocksdb::BlockBasedTableOptions& topts) {
  topts.block_size = 4 * 1024;
}

// Whole-key Bloom (10 bits/key) on the record CF. Separate from the helper above
// because secondary index CFs use a different filter or none, so this applies
// only to record-CF table options.
inline void ApplyRecordCfBloom(rocksdb::BlockBasedTableOptions& topts) {
  topts.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
}

}  // namespace experiment
