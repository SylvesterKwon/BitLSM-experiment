#include "lazy_bitmaps_binding.h"

#include <algorithm>
#include <chrono>
#include <cxxopts.hpp>
#include <filesystem>
#include <iostream>
#include <memory>
#include <rocksdb/filter_policy.h>
#include <rocksdb/iterator.h>
#include <rocksdb/perf_context.h>
#include <rocksdb/table.h>

#include "benchmark_experiment.h"
#include "bit_lsm_utils.h"
#include "lazy_bitmaps/lazy_bitmaps_keys.h"
#include "lazy_bitmaps/lazy_bitmaps_merge.h"
#include "lsm_stats.h"
#include "rocksdb_common_option.h"
#include "schema_loader.h"
#include "si_benchmark_common.h"

namespace experiment {

namespace {

[[noreturn]] void Fail(const std::string& what) {
  std::cerr << "[LazyBitmaps] " << what << "\n";
  exit(1);
}

}  // namespace

void LazyBitmapsBinding::Open(int argc, char* argv[],
                              const std::string& db_path,
                              const BitLSMOptions& opts) {
  options_ = opts;
  db_path_ = db_path;

  cxxopts::Options cxx("lazy-bitmaps", "");
  cxx.allow_unrecognised_options();
  cxx.add_options()
      ("rho", "Bin budget, as BitLSM's: 1/rho bins per attribute",
       cxxopts::value<double>()->default_value("0.001"))
      ("bin_policy",
       "Oracle bin policy file (global_bin_policy) for a new taxi DB",
       cxxopts::value<std::string>()->default_value(""))
      ("schema",
       "Benchmark schema JSON; a new synthetic DB bins uniformly over its ranges",
       cxxopts::value<std::string>()->default_value(""))
      ("max_background_jobs", "", cxxopts::value<int>()->default_value("6"))
      ("exp_type", "",
       cxxopts::value<std::string>()->default_value("write_seq"));
  auto result = cxx.parse(argc, argv);
  rho_ = result["rho"].as<double>();
  options_.rho = rho_;
  layout_.emplace(options_);
  rocksdb::Status s = bit_lsm::ValidateAttrSpecs(options_);
  if (!s.ok()) Fail(s.ToString());
  if (result["exp_type"].as<std::string>() == "write_seq_wa")
    stats_ = rocksdb::CreateDBStatistics();

  OpenDB(result["max_background_jobs"].as<int>());
  LoadOrCreatePolicy(result["bin_policy"].as<std::string>(),
                     result["schema"].as<std::string>());
  RecoverRowidCounter();
  std::cout << "[LazyBitmaps] opened " << db_path << ": policy "
            << binner_->policy().source << " (rho " << rho_ << ", "
            << binner_->policy().rows << " rows), next rowid "
            << next_rowid_.load() << "\n";
}

void LazyBitmapsBinding::OpenDB(int max_background_jobs) {
  // The setup of OpenSITransactionDB (si_benchmark_common.h) with one more
  // column family; kept inline because that helper opens exactly two.
  rocksdb::Options o;
  o.create_if_missing = true;
  o.create_missing_column_families = true;
  o.max_background_jobs = max_background_jobs;
  o.bytes_per_sync = 1048576;
  o.compaction_pri = rocksdb::kMinOverlappingRatio;
  o.max_write_buffer_number = 5;
  if (stats_) o.statistics = stats_;
  ApplyRocksdbCommonOptions(o);
  rocksdb::BlockBasedTableOptions base_table;
  ApplyRocksdbCommonTableOptions(base_table);
  o.table_factory.reset(rocksdb::NewBlockBasedTableFactory(base_table));

  // Record CF: whole-key Bloom, as every method's record CF.
  rocksdb::ColumnFamilyOptions primary(o);
  primary.level_compaction_dynamic_level_bytes = true;
  rocksdb::BlockBasedTableOptions primary_table;
  ApplyRocksdbCommonTableOptions(primary_table);
  ApplyRecordCfBloom(primary_table);
  primary.table_factory.reset(
      rocksdb::NewBlockBasedTableFactory(primary_table));

  // Index CFs: the options of Lazy Updates' secondary CF (no filter), plus
  // the OR merge operator on lazy_bitmaps. rowid_map keys are dense and
  // ascending, so every SST covers one contiguous rowid span and a point
  // lookup is pruned by file range without a Bloom.
  rocksdb::ColumnFamilyOptions index(o);
  index.level_compaction_dynamic_level_bytes = true;
  rocksdb::ColumnFamilyOptions bitmaps(index);
  bitmaps.merge_operator =
      std::make_shared<lazy_bitmaps::RoaringOrMergeOperator>();

  const std::vector<rocksdb::ColumnFamilyDescriptor> cfs = {
      {rocksdb::kDefaultColumnFamilyName, primary},
      {"lazy_bitmaps", bitmaps},
      {"rowid_map", index}};
  rocksdb::TransactionDBOptions txn_opts;
  rocksdb::Status s =
      rocksdb::TransactionDB::Open(o, txn_opts, db_path_, cfs, &cf_, &db_);
  if (!s.ok()) Fail("open " + db_path_ + ": " + s.ToString());
}

void LazyBitmapsBinding::LoadOrCreatePolicy(const std::string& policy_path,
                                            const std::string& schema_path) {
  const std::string sidecar = db_path_ + "/" + lazy_bitmaps::kSidecar;
  const bool have_sidecar = std::filesystem::exists(sidecar);
  auto policy = std::make_shared<global_bins::BinPolicy>();
  std::string error;
  if (have_sidecar) {
    if (!policy_path.empty())
      Fail("the DB already carries a bin policy (" + sidecar +
           "); drop --bin_policy");
    rocksdb::Status s = global_bins::LoadBinPolicy(sidecar, policy.get());
    if (!s.ok()) Fail(s.ToString());
  } else if (!policy_path.empty()) {
    rocksdb::Status s = global_bins::LoadBinPolicy(policy_path, policy.get());
    if (!s.ok()) Fail(s.ToString());
  } else if (!schema_path.empty()) {
    const Schema schema = load_schema(schema_path);
    if (!lazy_bitmaps::Binner::UniformPolicy(
            options_, schema.range_min, schema.range_max,
            schema.cardinalities, policy.get(), &error))
      Fail(error);
    policy->source = "uniform:" + schema_path;
  } else {
    Fail("a new DB needs --bin_policy (taxi) or --schema (synthetic)");
  }
  binner_ = lazy_bitmaps::Binner::FromPolicy(policy, options_, &error);
  if (!binner_) Fail(error);
  if (!have_sidecar) {
    rocksdb::Status s = global_bins::SaveBinPolicy(*policy, sidecar);
    if (!s.ok()) Fail(s.ToString());
  }
}

void LazyBitmapsBinding::RecoverRowidCounter() {
  // rowid_map is ordered by rowid, so its last key is the maximum ever
  // assigned; no per-put counter write is needed for durability.
  std::unique_ptr<rocksdb::Iterator> it(
      db_->NewIterator(rocksdb::ReadOptions(), cf_[kRowidMap]));
  it->SeekToLast();
  next_rowid_.store(
      it->Valid() ? lazy_bitmaps::RowidFromKey(it->key().ToStringView()) + 1
                  : 0);
}

void LazyBitmapsBinding::Put(const std::string& pk,
                             const std::vector<Attr>& attrs,
                             const std::string& payload) {
  thread_local std::string serialized_value, key, rowid_key;
  thread_local lazy_bitmaps::SingleRowidOperand operand;
  bit_lsm::EncodeValue(*layout_, attrs, payload, serialized_value);

  const uint64_t rowid = next_rowid_.fetch_add(1);
  if (rowid > UINT32_MAX) Fail("rowid overflow: bitmaps are 32-bit Roaring");
  lazy_bitmaps::RowidKey(rowid, &rowid_key);

  rocksdb::Transaction* txn = db_->BeginTransaction(wo_);
  txn->Put(cf_[kPrimary], pk, serialized_value);
  txn->Put(cf_[kRowidMap], rowid_key, pk);
  for (uint32_t i = 0; i < options_.attr_num; ++i) {
    if (std::holds_alternative<std::monostate>(attrs[i])) continue;  // NULL
    if (options_.attr_specs[i].index_type == IndexType::kEquality)
      lazy_bitmaps::EqualityKey(i, std::get<std::string>(attrs[i]), &key);
    else
      lazy_bitmaps::RangeBinKey(
          i, binner_->Bin(i, std::get<double>(attrs[i])), &key);
    txn->Merge(cf_[kBitmaps], key,
               operand.Bytes(static_cast<uint32_t>(rowid)));
  }
  txn->Commit();
  delete txn;
}

ScanResult LazyBitmapsBinding::Scan(BitLSMQuery& /*query*/) {
  return {0, 0};  // Task 5
}

WriteStats LazyBitmapsBinding::GetWriteStats() {
  if (!db_ || !stats_) return {};
  rocksdb::WaitForCompactOptions wfco;
  wfco.flush = true;
  wfco.wait_for_purge = true;
  db_->WaitForCompact(wfco);
  rocksdb::CompactRangeOptions cro;
  cro.bottommost_level_compaction =
      rocksdb::BottommostLevelCompaction::kForce;
  cro.exclusive_manual_compaction = true;
  for (auto* h : cf_) db_->CompactRange(cro, h, nullptr, nullptr);
  db_->WaitForCompact(wfco);
  return {stats_->getTickerCount(rocksdb::Tickers::FLUSH_WRITE_BYTES),
          stats_->getTickerCount(rocksdb::Tickers::COMPACT_WRITE_BYTES)};
}

IndexIoStats LazyBitmapsBinding::GetIndexIoStats() {
  IndexIoStats out;
  out.reads = idx_reads_.load();
  out.bytes = idx_bytes_.load();
  out.cache_hits = idx_hits_.load();
  out.cache_misses = idx_reads_.load();  // a block read is a cache miss
  return out;
}

void LazyBitmapsBinding::WaitForQuiescence() {
  if (!db_) return;
  rocksdb::WaitForCompactOptions wfco;
  wfco.flush = true;
  wfco.wait_for_purge = true;
  db_->WaitForCompact(wfco);
}

LsmStats LazyBitmapsBinding::SampleLsmStats() {
  return db_ ? SampleLsmStatsFrom(db_) : LsmStats{};
}

std::string LazyBitmapsBinding::ParamSuffix() const {
  return "_rho" + benchmark::format_double(rho_);
}

void LazyBitmapsBinding::Close() {
  if (!db_) return;
  // Per-CF on-disk size for the run log (Table 2 reports the sum and the
  // prose says how much is rowid_map).
  static const char* const kNames[] = {"primary", "lazy_bitmaps", "rowid_map"};
  std::cout << "[LazyBitmaps] sst bytes:";
  for (int i = 0; i < 3; ++i) {
    uint64_t v = 0;
    db_->GetIntProperty(cf_[i], rocksdb::DB::Properties::kTotalSstFilesSize,
                        &v);
    std::cout << " " << kNames[i] << "=" << v;
  }
  std::cout << ", values clamped to an edge bin: " << binner_->clamped()
            << ", next rowid " << next_rowid_.load() << "\n";
  for (auto* h : cf_) db_->DestroyColumnFamilyHandle(h);
  cf_.clear();
  rocksdb::WaitForCompactOptions wait_opts;
  wait_opts.close_db = true;
  db_->WaitForCompact(wait_opts);
  delete db_;
  db_ = nullptr;
  std::cout << "DB successfully closed\n";
}

}  // namespace experiment
