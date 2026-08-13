#pragma once
#include "binding.h"
#include "bit_lsm.h"
#include <memory>
#include <rocksdb/statistics.h>

namespace experiment {

class BitLSMBinding : public Binding {
  std::unique_ptr<bit_lsm::BitLSM> db_;
  double rho_ = 0.1;
  // "resident" materialises every bin's bitmap when a table opens; "ondemand"
  // reads a bin only when a query names it. Read path only -- the SST format
  // is identical, so one DB serves both.
  std::string index_mode_ = "resident";
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
