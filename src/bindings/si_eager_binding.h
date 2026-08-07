#pragma once
#include "binding.h"
#include "si_benchmark_common.h"
#include <memory>
#include <rocksdb/slice.h>
#include <rocksdb/statistics.h>
#include <string>
#include <vector>

namespace experiment {

class SIEagerBinding : public Binding {
  benchmark::SIDBHandles db_;
  BitLSMOptions options_;
  benchmark::SIStrategy strategy_ = benchmark::SIStrategy::kIndexMerge;
  rocksdb::WriteOptions wo_;
  std::shared_ptr<rocksdb::Statistics> stats_;

  static void InsertSIValue(std::vector<rocksdb::Slice>* si_value,
                            const rocksdb::Slice& key);

 public:
  void Open(int argc, char* argv[], const std::string& db_path,
            const BitLSMOptions& opts) override;
  void Put(const std::string& pk, const std::vector<Attr>& attrs,
           const std::string& payload) override;
  ScanResult Scan(BitLSMQuery& query) override;
  void Close() override;
  WriteStats GetWriteStats() override;
  void WaitForQuiescence() override;
  std::string Name() const override { return "si-eager"; }
  std::string ParamSuffix() const override;
};

}  // namespace experiment
