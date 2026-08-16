#pragma once
#include "binding.h"
#include "bit_lsm.h"
#include <memory>
#include <rocksdb/statistics.h>

namespace experiment {

class BitLSMBinding : public Binding {
  std::unique_ptr<bit_lsm::BitLSM> db_;
  double rho_ = 0.1;
  uint32_t scan_prefetch_depth_ = 0;
  std::shared_ptr<rocksdb::Statistics> stats_;

 public:
  void Open(int argc, char* argv[], const std::string& db_path,
            const BitLSMOptions& opts) override;
  void Put(const std::string& pk, const std::vector<Attr>& attrs,
           const std::string& payload) override;
  ScanResult Scan(BitLSMQuery& query) override;
  void Close() override;
  WriteStats GetWriteStats() override;
  void WaitForQuiescence() override;
  std::string Name() const override { return "bitlsm"; }
  std::string ParamSuffix() const override;
};

}  // namespace experiment
