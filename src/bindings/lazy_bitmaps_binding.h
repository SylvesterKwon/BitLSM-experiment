#pragma once
// Lazy Bitmaps: Lazy Updates with Roaring-bitmap postings. A standalone
// secondary index in its own column family, maintained lazily (every put
// appends the operand {rowid} under each (attr, bin) key; nothing is read or
// rewritten) and folded by bitwise OR at flush, compaction and lookup, in the
// manner of Weaviate's RoaringSet. Records are addressed by a monotonically
// assigned rowid that rowid_map resolves to the primary key. An update appends
// a new rowid and leaves the old bits in place, as Lazy Updates leaves
// obsolete postings; freshness validation at read time discards them.
// Continuous attributes are binned with BitLSM's rule at the same rho, with
// global (oracle) boundaries, so the comparison with BitLSM isolates where the
// bitmap lives (global CF vs SST-local).
//
// Column families: default = PK -> record (as every other method),
// lazy_bitmaps = (attr, bin) -> bitmap (Merge only, OR), rowid_map = rowid ->
// PK (Put). See lazy_bitmaps/ for the encodings, the merge operator and the
// binner.
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <rocksdb/statistics.h>
#include <rocksdb/utilities/transaction_db.h>

#include "binding.h"
#include "lazy_bitmaps/lazy_bitmaps_binner.h"

namespace experiment {

class LazyBitmapsBinding : public Binding {
 public:
  struct ScanCounts {
    uint64_t q = 0;        // rowids after the AND (bin false positives + stale)
    uint64_t pks = 0;      // distinct PKs after rowid_map + dedup
    uint64_t matched = 0;  // records that pass the full CNF
  };

  void Open(int argc, char* argv[], const std::string& db_path,
            const BitLSMOptions& opts) override;
  void Put(const std::string& pk, const std::vector<Attr>& attrs,
           const std::string& payload) override;
  ScanResult Scan(BitLSMQuery& query) override;
  void Close() override;
  WriteStats GetWriteStats() override;
  IndexIoStats GetIndexIoStats() override;
  void WaitForQuiescence() override;
  LsmStats SampleLsmStats() override;
  std::string Name() const override { return "lazy-bitmaps"; }
  std::string ParamSuffix() const override;

  // Test hooks.
  uint64_t NextRowid() const { return next_rowid_.load(); }
  ScanCounts LastScanCounts() const { return last_scan_; }

 private:
  static constexpr int kPrimary = 0, kBitmaps = 1, kRowidMap = 2;

  void OpenDB(int max_background_jobs);
  void LoadOrCreatePolicy(const std::string& policy_path,
                          const std::string& schema_path);
  void RecoverRowidCounter();

  rocksdb::TransactionDB* db_ = nullptr;
  std::vector<rocksdb::ColumnFamilyHandle*> cf_;
  BitLSMOptions options_;
  // Built once in Open(); see no_index_binding.h.
  std::optional<bit_lsm::ValueLayout> layout_;
  std::unique_ptr<lazy_bitmaps::Binner> binner_;
  std::atomic<uint64_t> next_rowid_{0};
  double rho_ = 0.001;
  std::string db_path_;
  rocksdb::WriteOptions wo_;
  std::shared_ptr<rocksdb::Statistics> stats_;
  // Index I/O (lazy_bitmaps + rowid_map) since Open; honk_player deltas it
  // across each Scan().
  std::atomic<uint64_t> idx_reads_{0}, idx_bytes_{0}, idx_hits_{0};
  ScanCounts last_scan_;
};

}  // namespace experiment
