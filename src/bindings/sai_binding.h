#pragma once
#include "binding.h"
#include "sai_db.h"
#include <memory>
#include <rocksdb/statistics.h>

namespace experiment {

// RocksDB structural-equivalent port of Cassandra SAI (Storage-Attached Index).
// Design/deviations: external plans doc 2026-07-22-sai-baseline-design.md.
class SAIBinding : public Binding {
  std::unique_ptr<sai::SAIDB> db_;
  // Cassandra default: cassandra.sai.intersection_clause_limit = 2
  // (config/CassandraRelevantProperties.java:489). <=0 means "intersect all".
  int intersection_limit_ = 2;
  // "resident" keeps the whole per-SSTable blob in memory once loaded (design
  // deviation D5); "ondemand" reads blob ranges per lookup, which is what
  // Cassandra does. Only the read path differs -- the on-disk format is the
  // same, so one DB serves both modes.
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
  std::string Name() const override { return "sai"; }
  std::string ParamSuffix() const override {
    // The mode only shows up in the suffix when it is not the default, so
    // existing result files keep their names.
    return "_il" + std::to_string(intersection_limit_) +
           (index_mode_ == "resident" ? "" : "_" + index_mode_);
  }
};

}  // namespace experiment
