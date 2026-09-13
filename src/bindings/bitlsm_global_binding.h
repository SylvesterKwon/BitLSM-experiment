#pragma once
// BitLSM with global (oracle) bin boundaries: the ablation arm for local
// binning. Not a system of its own -- the same BitLSM with one input to the
// SABI builder changed. See global_bins/global_sabi_builder.h.
//
// The core opens every column family with its own SABIFactory, so the builder
// cannot be swapped under bit_lsm::BitLSM. The binding therefore has two
// modes, chosen by --bin_policy:
//   build (--bin_policy <file>): a plain RocksDB DB opened with BitLSM's own
//     options and a GlobalSABIFactory; Put encodes rows exactly as
//     BitLSM::Put does. Close records the live SST set.
//   query (no --bin_policy): the unmodified bit_lsm::BitLSM, exactly as
//     BitLSMBinding opens it. The SSTs carry ordinary v8 SABI blobs, so every
//     read goes through the core reader and iterators.
// Opening under BitLSM runs the core builder on any flush or compaction, which
// would silently rewrite SSTs with local bins. Query mode therefore checks the
// live SST set against the recorded one at Open and at Close and aborts on any
// difference.
#include <memory>
#include <string>

#include <rocksdb/db.h>

#include "bitlsm_binding.h"
#include "bit_lsm_utils.h"

namespace experiment {

class BitLSMGlobalBinding : public BitLSMBinding {
  // Build mode only.
  std::unique_ptr<rocksdb::DB> raw_db_;
  rocksdb::ColumnFamilyHandle* raw_cf_ = nullptr;
  rocksdb::Options raw_options_;
  rocksdb::ColumnFamilyOptions raw_cf_options_;
  std::unique_ptr<bit_lsm::ValueLayout> layout_;
  BitLSMOptions build_opts_{};
  std::string serialized_value_;
  std::string db_path_;

  bool BuildMode() const { return raw_db_ != nullptr; }
  void OpenRaw();
  void CloseRaw();

 public:
  void Open(int argc, char* argv[], const std::string& db_path,
            const BitLSMOptions& opts) override;
  void Put(const std::string& pk, const std::vector<Attr>& attrs,
           const std::string& payload) override;
  ScanResult Scan(BitLSMQuery& query) override;
  void Close() override;
  WriteStats GetWriteStats() override { return {}; }
  void WaitForQuiescence() override;
  std::string Name() const override { return "bitlsm-global"; }
};

}  // namespace experiment
