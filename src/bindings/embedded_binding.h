#pragma once
#include "binding.h"
#include "embedded_db.h"
#include <memory>
#include <rocksdb/statistics.h>

namespace experiment {

class EmbeddedBinding : public Binding {
  std::unique_ptr<embedded::EmbeddedDB> db_;
  uint32_t bloom_bits_ = 100;
  std::shared_ptr<rocksdb::Statistics> stats_;

 public:
  void Open(int argc, char* argv[], const std::string& db_path,
            const BitLSMOptions& opts) override;
  void Put(const std::string& pk, const std::vector<Attr>& attrs,
           const std::string& payload) override;
  ScanResult Scan(BitLSMQuery& query) override;
  void Close() override;
  WriteStats GetWriteStats() override;
  std::string Name() const override { return "embedded"; }
  std::string ParamSuffix() const override {
    return "_bits" + std::to_string(bloom_bits_);
  }
};

}  // namespace experiment
