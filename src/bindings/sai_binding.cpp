#include "sai_binding.h"
#include "benchmark_experiment.h"
#include "sai_iterator.h"
#include "blob_source.h"
#include "rocksdb_common_option.h"
#include <chrono>
#include <cxxopts.hpp>
#include <iostream>
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
                   ("scan_prefetch_depth",
                    "Scan data-block reads kept in flight (0 = one at a time)",
                    cxxopts::value<uint32_t>()->default_value("0"))
                   ("index_mode", "resident (default) or ondemand",
                    cxxopts::value<std::string>()->default_value("resident"))
                   ("exp_type", "",
                    cxxopts::value<std::string>()->default_value("write_seq"));
  auto result = cxx.parse(argc, argv);
  scan_prefetch_depth_ = result["scan_prefetch_depth"].as<uint32_t>();
  intersection_limit_ = result["intersection_limit"].as<int>();
  const std::string index_mode = result["index_mode"].as<std::string>();
  if (index_mode != "resident" && index_mode != "ondemand") {
    std::cerr << "[SAIBinding] invalid --index_mode '" << index_mode
              << "' (expected resident or ondemand)\n";
    exit(1);
  }
  ondemand_index_ = (index_mode == "ondemand");
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
  ApplyRocksdbCommonTableOptions(table_options);
  ApplyRecordCfBloom(table_options);

  BitLSMOptions scan_opts = opts;
  scan_opts.scan_prefetch_depth = scan_prefetch_depth_;
  scan_opts.ondemand_index = ondemand_index_;
  db_ = std::make_unique<sai::SAIDB>(db_path, scan_opts, rocksdb_options,
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
  // !Valid() alone only means "no more rows"; a non-OK status means the scan
  // stopped on an error and `matched` is a partial count.
  rocksdb::Status s = iter->status();
  if (!s.ok()) {
    std::cerr << "[SAIBinding::Scan] scan failed: " << s.ToString() << "\n";
    return {static_cast<uint64_t>(elapsed), matched, false};
  }
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

void SAIBinding::WaitForQuiescence() {
  if (!db_) return;
  rocksdb::WaitForCompactOptions wfco;
  wfco.flush = true;
  wfco.wait_for_purge = true;
  db_->GetInternalDB()->WaitForCompact(wfco);
}

void SAIBinding::Close() {
  if (ondemand_index_) {
    BlobSourceStats stats = GetBlobSourceStats();
    std::cerr << "[SAIBinding] ondemand blob-page stats: hits=" << stats.page_hits
              << " misses=" << stats.page_misses
              << " bytes_read=" << stats.bytes_read
              << " span_reads=" << stats.span_reads << "\n";
  }
  db_.reset();
}

}  // namespace experiment
