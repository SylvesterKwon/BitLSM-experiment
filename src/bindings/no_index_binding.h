#pragma once
#include "binding.h"
#include <memory>
#include <optional>
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/statistics.h>
#include <string>
#include <vector>

namespace experiment {

class NoIndexBinding : public Binding {
  rocksdb::DB* db_ = nullptr;
  std::vector<rocksdb::ColumnFamilyHandle*> cf_handles_;
  BitLSMOptions options_;
  // Built once in Open(). The row-level value API takes a cached layout
  // because building one costs four vector allocations -- per record in Put(),
  // per row in a scan's predicate check.
  std::optional<bit_lsm::ValueLayout> layout_;
  rocksdb::WriteOptions wo_;
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
  std::string Name() const override { return "no-index"; }
};

}  // namespace experiment
