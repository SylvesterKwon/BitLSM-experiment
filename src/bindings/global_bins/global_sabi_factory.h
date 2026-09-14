#pragma once
// UDI factory that builds SSTs with GlobalSABIBuilder and reads them with the
// core SABIReader.
//
// Name() is "SABIFactory" on purpose: RocksDB stores the index block under
// "rocksdb.user_defined_index." + Name() and looks it up by the opening
// factory's name, so SSTs written here open unchanged under bit_lsm::BitLSM.
#include <memory>

#include "bin_policy.h"
#include "sabi.h"

namespace experiment::global_bins {

class GlobalSABIFactory : public rocksdb::UserDefinedIndexFactory {
 public:
  // Aborts if the policy was not built for this schema.
  GlobalSABIFactory(const bit_lsm::BitLSMOptions& options,
                    std::shared_ptr<const BinPolicy> policy);

  const char* Name() const override { return "SABIFactory"; }
  rocksdb::UserDefinedIndexBuilder* NewBuilder() const override;
  std::unique_ptr<rocksdb::UserDefinedIndexReader> NewReader(
      rocksdb::Slice& index_block) const override;
  rocksdb::Status NewReader(
      const rocksdb::UserDefinedIndexOption& option,
      rocksdb::Slice& index_block,
      std::unique_ptr<rocksdb::UserDefinedIndexReader>& reader) const override;

 private:
  bit_lsm::BitLSMOptions options_;
  bit_lsm::SABISchema schema_;
  std::shared_ptr<const BinPolicy> policy_;
  // Reader-only core factory: flush and compaction open their input SSTs, and
  // those readers must be exactly the core's.
  bit_lsm::SABIFactory reader_factory_;
};

}  // namespace experiment::global_bins
