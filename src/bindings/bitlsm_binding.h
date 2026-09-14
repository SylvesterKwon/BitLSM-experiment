#pragma once
#include "binding.h"
#include "bit_lsm.h"
#include <memory>
#include <rocksdb/statistics.h>

namespace experiment {

class BitLSMBinding : public Binding {
 protected:
  std::unique_ptr<bit_lsm::BitLSM> db_;
  double rho_ = 0.001;
  uint32_t scan_prefetch_depth_ = 0;
  bool ondemand_index_ = false;
  std::shared_ptr<rocksdb::Statistics> stats_;

  // Parses the BitLSM flags into the members above and fills the options a
  // BitLSM DB opens with. Split out of Open() so BitLSMGlobalBinding writes its
  // DB with exactly these options.
  void BuildOpenOptions(int argc, char* argv[], const BitLSMOptions& opts,
                        rocksdb::Options* rocksdb_options,
                        rocksdb::BlockBasedTableOptions* table_options,
                        BitLSMOptions* bitlsm_opts);

 public:
  void Open(int argc, char* argv[], const std::string& db_path,
            const BitLSMOptions& opts) override;
  void Put(const std::string& pk, const std::vector<Attr>& attrs,
           const std::string& payload) override;
  ScanResult Scan(BitLSMQuery& query) override;
  void Close() override;
  WriteStats GetWriteStats() override;
  IndexIoStats GetIndexIoStats() override;
  void WaitForQuiescence() override;
  std::string Name() const override { return "bitlsm"; }
  std::string ParamSuffix() const override;
};

}  // namespace experiment
