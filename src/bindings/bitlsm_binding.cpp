#include "bitlsm_binding.h"
#include "benchmark_experiment.h"
#include <chrono>
#include <cxxopts.hpp>
#include <iostream>
#include "rocksdb_common_option.h"
#include "sabi.h"
#include <rocksdb/filter_policy.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>

namespace experiment {

void BitLSMBinding::Open(int argc, char* argv[], const std::string& db_path,
                          const BitLSMOptions& opts) {
  cxxopts::Options cxx("bitlsm", "");
  cxx.allow_unrecognised_options();
  cxx.add_options()("rho", "BitLSM rho threshold",
                    cxxopts::value<double>()->default_value("0.001"))
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
  rho_ = result["rho"].as<double>();
  const std::string index_mode = result["index_mode"].as<std::string>();
  if (index_mode != "resident" && index_mode != "ondemand") {
    std::cerr << "[BitLSMBinding] invalid --index_mode '" << index_mode
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

  BitLSMOptions bitlsm_opts = opts;
  bitlsm_opts.rho = rho_;
  bitlsm_opts.scan_prefetch_depth = scan_prefetch_depth_;
  bitlsm_opts.ondemand_index = ondemand_index_;
  db_ = std::make_unique<bit_lsm::BitLSM>(db_path, bitlsm_opts,
                                            rocksdb_options, table_options);
  // Zero the process-wide on-demand bin cache counters so this run's totals
  // (and the per-query deltas honk_player computes from them) don't carry
  // noise from whatever touched them before this DB was opened.
  bit_lsm::ResetSABIBinCacheStats();
}

void BitLSMBinding::Put(const std::string& pk, const std::vector<Attr>& attrs,
                         const std::string& payload) {
  db_->Put(pk, attrs, payload);
}

ScanResult BitLSMBinding::Scan(BitLSMQuery& query) {
  uint64_t matched = 0;
  auto start = std::chrono::high_resolution_clock::now();
  auto iter = db_->NewIterator(query);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next())
    matched++;
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::high_resolution_clock::now() - start)
                     .count();
  // !Valid() alone only means "no more rows"; a non-OK status means the scan
  // stopped on an error (typically an SST whose SABI block could not be
  // loaded) and `matched` is a partial count.
  rocksdb::Status s = iter->status();
  if (!s.ok()) {
    std::cerr << "[BitLSMBinding::Scan] scan failed: " << s.ToString() << "\n";
    return {static_cast<uint64_t>(elapsed), matched, false};
  }
  return {static_cast<uint64_t>(elapsed), matched};
}

WriteStats BitLSMBinding::GetWriteStats() {
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

void BitLSMBinding::WaitForQuiescence() {
  if (!db_) return;
  rocksdb::WaitForCompactOptions wfco;
  wfco.flush = true;
  wfco.wait_for_purge = true;
  db_->GetInternalDB()->WaitForCompact(wfco);
}

IndexIoStats BitLSMBinding::GetIndexIoStats() {
  bit_lsm::SABIBinCacheStats s = bit_lsm::GetSABIBinCacheStats();
  IndexIoStats out;
  // Device reads issued: synchronous preads (reads) plus async span
  // submissions that ended up serving the miss (spans_prefetched) -- a
  // spans_prefetched run adds bytes_read but no `reads`, so it must be added
  // in separately to count as a device request.
  out.reads = s.reads + s.spans_prefetched;
  out.bytes = s.bytes_read;
  out.cache_hits = s.hits;
  out.cache_misses = s.misses;
  return out;
}

void BitLSMBinding::Close() {
  if (ondemand_index_) {
    bit_lsm::SABIBinCacheStats stats = bit_lsm::GetSABIBinCacheStats();
    std::cerr << "[BitLSMBinding] ondemand SABI bin-cache stats: hits="
              << stats.hits << " misses=" << stats.misses
              << " reads=" << stats.reads
              << " bytes_read=" << stats.bytes_read
              << " bitmaps_loaded=" << stats.bitmaps_loaded
              << " inserts_refused=" << stats.inserts_refused
              << " spans_planned=" << stats.spans_planned
              << " spans_prefetched=" << stats.spans_prefetched
              << " spans_dropped=" << stats.spans_dropped << "\n";
  }
  db_.reset();
}

std::string BitLSMBinding::ParamSuffix() const {
  return "_rho" + benchmark::format_double(rho_) +
         (ondemand_index_ ? "_ondemand" : "");
}

}  // namespace experiment
