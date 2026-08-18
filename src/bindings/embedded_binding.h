#pragma once
#include "binding.h"
#include "embedded_db.h"
#include <memory>
#include <rocksdb/statistics.h>

namespace experiment {

class EmbeddedBinding : public Binding {
  std::unique_ptr<embedded::EmbeddedDB> db_;
  // de facto standard bits-per-key (~1% false positive), matching LevelDB/
  // RocksDB and si_ck_binding's NewBloomFilterPolicy(10). FP depends on the
  // bits-per-key ratio, not the set size, so this is set-size-independent.
  // (Qader 2018 used 100, retained as a sweepable point via --bloom_bits.)
  uint32_t bloom_bits_ = 10;
  uint32_t scan_prefetch_depth_ = 0;
  bool ondemand_index_ = false;
  std::shared_ptr<rocksdb::Statistics> stats_;

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
  std::string Name() const override { return "embedded"; }
  std::string ParamSuffix() const override {
    return "_bloom_bits" + std::to_string(bloom_bits_) +
           (ondemand_index_ ? "_ondemand" : "");
  }
};

}  // namespace experiment
