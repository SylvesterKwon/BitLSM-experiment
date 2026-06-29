#include "embedded_binding.h"
#include "benchmark_experiment.h"
#include <chrono>
#include <cxxopts.hpp>
#include <rocksdb/options.h>
#include <rocksdb/table.h>

namespace experiment {

void EmbeddedBinding::Open(int argc, char* argv[], const std::string& db_path,
                            const BitLSMOptions& opts) {
  cxxopts::Options cxx("embedded", "");
  cxx.allow_unrecognised_options();
  cxx.add_options()("bloom_bits", "Embedded index bloom bits per key",
                    cxxopts::value<uint32_t>()->default_value("10"))
                   ("max_background_jobs", "",
                    cxxopts::value<int>()->default_value("6"))
                   ("exp_type", "",
                    cxxopts::value<std::string>()->default_value("write_seq"));
  auto result = cxx.parse(argc, argv);
  bloom_bits_ = result["bloom_bits"].as<uint32_t>();
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
  rocksdb::BlockBasedTableOptions table_options;
  table_options.block_size = 4 * 1024;

  db_ = std::make_unique<embedded::EmbeddedDB>(db_path, opts, rocksdb_options,
                                                table_options, bloom_bits_);
}

void EmbeddedBinding::Put(const std::string& pk, const std::vector<Attr>& attrs,
                           const std::string& payload) {
  db_->Put(pk, attrs, payload);
}

ScanResult EmbeddedBinding::Scan(BitLSMQuery& query) {
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

WriteStats EmbeddedBinding::GetWriteStats() {
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

void EmbeddedBinding::Close() { db_.reset(); }

}  // namespace experiment
