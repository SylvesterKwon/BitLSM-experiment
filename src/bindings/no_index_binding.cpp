#include "no_index_binding.h"
#include "bit_lsm_utils.h"
#include "rocksdb_common_option.h"
#include <chrono>
#include <cxxopts.hpp>
#include <iostream>
#include <rocksdb/filter_policy.h>
#include <rocksdb/table.h>

using namespace rocksdb;

namespace experiment {

void NoIndexBinding::Open(int argc, char* argv[], const std::string& db_path,
                           const BitLSMOptions& opts) {
  options_ = opts;
  layout_.emplace(options_);

  cxxopts::Options cxx("no-index", "");
  cxx.allow_unrecognised_options();
  cxx.add_options()("max_background_jobs", "",
                    cxxopts::value<int>()->default_value("6"))
                   ("exp_type", "",
                    cxxopts::value<std::string>()->default_value("write_seq"));
  auto result = cxx.parse(argc, argv);

  bool wa_mode = (result["exp_type"].as<std::string>() == "write_seq_wa");

  Options rocksdb_options;
  rocksdb_options.create_if_missing = true;
  rocksdb_options.max_background_jobs = result["max_background_jobs"].as<int>();
  rocksdb_options.bytes_per_sync = 1048576;
  rocksdb_options.compaction_pri = kMinOverlappingRatio;
  rocksdb_options.max_write_buffer_number = 5;
  if (wa_mode) {
    stats_ = CreateDBStatistics();
    rocksdb_options.statistics = stats_;
  }
  ApplyRocksdbCommonOptions(rocksdb_options);
  BlockBasedTableOptions table_options;
  ApplyRocksdbCommonTableOptions(table_options);
  ApplyRecordCfBloom(table_options);
  rocksdb_options.table_factory.reset(
      NewBlockBasedTableFactory(table_options));

  ColumnFamilyOptions cf_opts(rocksdb_options);
  cf_opts.level_compaction_dynamic_level_bytes = true;
  const std::vector<ColumnFamilyDescriptor> column_families(
      {ColumnFamilyDescriptor(kDefaultColumnFamilyName, cf_opts)});

  Status s = DB::Open(rocksdb_options, db_path, column_families,
                      &cf_handles_, &db_);
  if (!s.ok()) {
    std::cerr << "Failed to open DB: " << s.ToString() << "\n";
    exit(1);
  }
}

void NoIndexBinding::Put(const std::string& pk,
                          const std::vector<Attr>& attrs,
                          const std::string& payload) {
  thread_local std::string serialized_value;
  EncodeValue(*layout_, attrs, payload, serialized_value);
  db_->Put(wo_, pk, serialized_value);
}

ScanResult NoIndexBinding::Scan(BitLSMQuery& query) {
  ReadOptions ro;
  auto* it = db_->NewIterator(ro);
  uint64_t matched = 0;
  uint64_t total = 0;
  auto start = std::chrono::high_resolution_clock::now();
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    total++;
    if (query.CheckCondition(it->value(), *layout_))
      matched++;
  }
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::high_resolution_clock::now() - start)
                     .count();
  delete it;
  std::cout << "scan done: " << matched << "/" << total << " matched, "
            << elapsed << "ms\n";
  return {static_cast<uint64_t>(elapsed), matched};
}

WriteStats NoIndexBinding::GetWriteStats() {
  if (!db_ || !stats_) return {};
  WaitForCompactOptions wfco;
  wfco.flush = true;
  wfco.wait_for_purge = true;
  db_->WaitForCompact(wfco);
  CompactRangeOptions cro;
  cro.bottommost_level_compaction = BottommostLevelCompaction::kForce;
  cro.exclusive_manual_compaction = true;
  db_->CompactRange(cro, nullptr, nullptr);
  db_->WaitForCompact(wfco);
  return {stats_->getTickerCount(Tickers::FLUSH_WRITE_BYTES),
          stats_->getTickerCount(Tickers::COMPACT_WRITE_BYTES)};
}

void NoIndexBinding::WaitForQuiescence() {
  if (!db_) return;
  WaitForCompactOptions wfco;
  wfco.flush = true;
  wfco.wait_for_purge = true;
  db_->WaitForCompact(wfco);
}

void NoIndexBinding::Close() {
  if (!db_) return;
  if (!cf_handles_.empty()) {
    for (auto* h : cf_handles_)
      db_->DestroyColumnFamilyHandle(h);
    cf_handles_.clear();
  }
  WaitForCompactOptions wait_opts;
  wait_opts.close_db = true;
  db_->WaitForCompact(wait_opts);
  delete db_;
  db_ = nullptr;
  std::cout << "DB successfully closed\n";
}

}  // namespace experiment
