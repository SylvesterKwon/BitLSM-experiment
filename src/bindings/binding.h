#pragma once
#include "bit_lsm.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace experiment {

using ::Attr;  // global scope (bit_lsm.h)
using bit_lsm::IndexType;
using bit_lsm::BitLSMOptions;
using bit_lsm::BitLSMQuery;

struct ScanResult {
  uint64_t elapsed_ms;
  uint64_t matched;
  // False when the scan stopped on an error (e.g. an index block that could
  // not be loaded): `matched` is then a partial count and must not be
  // recorded as a result. Bindings without status plumbing leave it true.
  bool ok = true;
};

struct WriteStats {
  uint64_t flush_bytes = 0;
  uint64_t compact_bytes = 0;
};

// Method-comparable index-I/O totals, read per query (honk_player computes
// the delta across each Scan()). Uniform across bindings: `reads`/`bytes`
// are device I/O requests/bytes issued to serve index lookups, `cache_hits`/
// `cache_misses` are index-cache lookups. Method-specific counters (e.g.
// BitLSM's bitmaps_loaded) stay internal to each binding's own mapping --
// this struct is the CSV-facing schema only. Hit/miss COUNTS are
// unit-different across methods (bin vs page/extent granularity), so only
// rates and bytes are directly comparable across bindings; see the mapping
// comments in bitlsm_binding.cpp / sai_binding.cpp / embedded_binding.cpp.
// Absolute, process-wide totals -- not per-query on their own.
struct IndexIoStats {
  uint64_t reads = 0;
  uint64_t bytes = 0;
  uint64_t cache_hits = 0;
  uint64_t cache_misses = 0;
};

class Binding {
 public:
  virtual ~Binding() = default;

  virtual void Open(int argc, char* argv[], const std::string& db_path,
                    const BitLSMOptions& opts) = 0;

  virtual void Put(const std::string& pk, const std::vector<Attr>& attrs,
                   const std::string& payload) = 0;

  virtual ScanResult Scan(BitLSMQuery& query) = 0;

  virtual void Close() = 0;

  virtual std::string Name() const = 0;

  virtual std::string ParamSuffix() const { return ""; }

  // For WA experiment: flush memtable, wait for compactions to settle, and
  // return cumulative flush/compaction write bytes from RocksDB statistics.
  // Returns {0,0} when statistics is not enabled.
  virtual WriteStats GetWriteStats() { return {}; }

  // Absolute index-I/O totals since the last reset (each override resets its
  // underlying counters once at Open()). Default covers bindings with no
  // separate index I/O machinery (no-index, si-*): always zero.
  virtual IndexIoStats GetIndexIoStats() { return {}; }

  // Flush the memtable and wait until all ingestion-induced background work
  // (compactions, obsolete-file purges) has drained. Unlike GetWriteStats()
  // this schedules no manual compaction: the LSM settles into whatever shape
  // the ingest itself produced. Write experiments call this before Close()
  // so that measured wall time / CPU / DB size cover the full induced work.
  virtual void WaitForQuiescence() {}
};

std::unique_ptr<Binding> CreateBinding(const std::string& name);

}  // namespace experiment
