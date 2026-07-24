#include "sai_binding.h"
#include "benchmark_experiment.h"
#include "sai_iterator.h"
#include "rocksdb_common_option.h"
#include <chrono>
#include <cxxopts.hpp>
#include <rocksdb/filter_policy.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>

namespace experiment {

void SAIBinding::Open(int argc, char* argv[], const std::string& db_path,
                      const BitLSMOptions& opts) {
  cxxopts::Options cxx("sai", "");
  cxx.allow_unrecognised_options();
  cxx.add_options()("intersection_limit",
                    "Max predicates joined by index intersection (<=0: all)",
                    cxxopts::value<int>()->default_value("2"))
                   ("max_background_jobs", "",
                    cxxopts::value<int>()->default_value("6"))
                   ("exp_type", "",
                    cxxopts::value<std::string>()->default_value("write_seq"));
  auto result = cxx.parse(argc, argv);
  intersection_limit_ = result["intersection_limit"].as<int>();
  bool wa_mode = (result["exp_type"].as<std::string>() == "write_seq_wa");

  rocksdb::Options rocksdb_options;
  rocksdb_options.create_if_missing = true;
  rocksdb_options.max_background_jobs = result["max_background_jobs"].as<int>();
  rocksdb_options.bytes_per_sync = 1048576;
  rocksdb_options.compaction_pri = rocksdb::kMinOverlappingRatio;
  rocksdb_options.max_write_buffer_number = 5;
  if (wa_mode) {
    stats_ = rocksdb::CreateDBStatistics();
    rocksdb_options.statistics = stats_;
  }
  ApplyRocksdbCommonOptions(rocksdb_options);

  rocksdb::BlockBasedTableOptions table_options;
  table_options.block_size = 4 * 1024;
  ApplyRocksdbCommonTableOptions(table_options, MakeExperimentBlockCache());
  // Whole-key Bloom on the record CF, the controlled constant shared by all
  // methods (commit 0f1c75f). SAIIterator resolves index candidates via
  // MultiGet on this CF, so it sits on the hot read path here as it does for
  // bitlsm.
  table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));

  db_ = std::make_unique<sai::SAIDB>(db_path, opts, rocksdb_options,
                                     table_options, intersection_limit_);
}

void SAIBinding::Put(const std::string& pk, const std::vector<Attr>& attrs,
                     const std::string& payload) {
  db_->Put(pk, attrs, payload);
}

ScanResult SAIBinding::Scan(BitLSMQuery& query) {
  uint64_t matched = 0;
  auto start = std::chrono::high_resolution_clock::now();
  auto iter = db_->NewIterator(query);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next())
    matched++;
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::high_resolution_clock::now() - start)
                     .count();
  return {static_cast<uint64_t>(elapsed), matched};
}

WriteStats SAIBinding::GetWriteStats() {
  if (!db_ || !stats_) return {};
  auto* raw = db_->GetInternalDB();
  rocksdb::WaitForCompactOptions wfco;
  wfco.flush = true;
  wfco.wait_for_purge = true;
  raw->WaitForCompact(wfco);
  rocksdb::CompactRangeOptions cro;
  cro.bottommost_level_compaction =
      rocksdb::BottommostLevelCompaction::kForce;
  cro.exclusive_manual_compaction = true;
  raw->CompactRange(cro, nullptr, nullptr);
  raw->WaitForCompact(wfco);
  return {stats_->getTickerCount(rocksdb::Tickers::FLUSH_WRITE_BYTES),
          stats_->getTickerCount(rocksdb::Tickers::COMPACT_WRITE_BYTES)};
}

void SAIBinding::Close() { db_.reset(); }

}  // namespace experiment
