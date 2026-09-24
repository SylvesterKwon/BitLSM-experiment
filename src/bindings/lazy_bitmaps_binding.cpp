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

namespace {

// Thread-local RocksDB counters, on at the default perf level, so per-phase
// deltas give the index bytes without a Statistics object.
struct PerfSnapshot {
  uint64_t reads, bytes, hits;
  static PerfSnapshot Now() {
    const rocksdb::PerfContext* pc = rocksdb::get_perf_context();
    return {pc->block_read_count, pc->block_read_byte,
            pc->block_cache_hit_count};
  }
  PerfSnapshot operator-(const PerfSnapshot& o) const {
    return {reads - o.reads, bytes - o.bytes, hits - o.hits};
  }
};

constexpr size_t kMultiGetBatch = 1000000;  // as ScanByIndexMerge

}  // namespace

ScanResult LazyBitmapsBinding::Scan(BitLSMQuery& query) {
  auto plan = benchmark::MapQueryToSILookups(query, options_);
  if (plan.si_lookups.empty()) {
    auto r = benchmark::ScanFullTable(db_, cf_, query, options_, 0);
    last_scan_ = {r.records_matched, r.records_matched, r.records_matched};
    return {r.time_elapsed_ms, r.records_matched};
  }
  const auto start = std::chrono::high_resolution_clock::now();
  const PerfSnapshot p0 = PerfSnapshot::Now();
  lazy_bitmaps::tl_operands_folded = 0;

  // One snapshot for the three column families, so the reads cannot drift
  // apart under a concurrent writer.
  const rocksdb::Snapshot* snap = db_->GetSnapshot();
  rocksdb::ReadOptions ro;
  ro.snapshot = snap;

  // 1. AND over the predicates of the OR over each predicate's bins. Get and
  // the iterator return the fully folded bitmap; that fold is part of the
  // query and is never cached across queries.
  roaring::Roaring q;
  bool first = true;
  std::string key, upper;
  for (const auto& lookup : plan.si_lookups) {
    roaring::Roaring b;
    if (lookup.type == benchmark::SILookupType::kPointLookup) {
      for (const auto& value : lookup.sk_values) {
        lazy_bitmaps::EqualityKey(lookup.attr_idx, value, &key);
        rocksdb::PinnableSlice bytes;
        rocksdb::Status s = db_->Get(ro, cf_[kBitmaps], key, &bytes);
        if (s.ok())
          b |= lazy_bitmaps::ReadBitmap(bytes.ToStringView());
        else if (!s.IsNotFound())
          Fail("bitmap Get: " + s.ToString());
      }
    } else {
      const uint32_t lo =
          lookup.lower_bound
              ? binner_->Bin(lookup.attr_idx, *lookup.lower_bound)
              : 0;
      const uint32_t hi =
          lookup.upper_bound
              ? binner_->Bin(lookup.attr_idx, *lookup.upper_bound)
              : binner_->Bins(lookup.attr_idx) - 1;
      lazy_bitmaps::RangeBinKey(lookup.attr_idx, lo, &key);
      lazy_bitmaps::RangeBinKey(lookup.attr_idx, hi + 1, &upper);
      rocksdb::ReadOptions span = ro;
      const rocksdb::Slice upper_slice(upper);
      span.iterate_upper_bound = &upper_slice;
      std::unique_ptr<rocksdb::Iterator> it(
          db_->NewIterator(span, cf_[kBitmaps]));
      for (it->Seek(key); it->Valid(); it->Next())
        b |= lazy_bitmaps::ReadBitmap(it->value().ToStringView());
      if (!it->status().ok()) Fail("bitmap span: " + it->status().ToString());
    }
    if (first) {
      q = std::move(b);
      first = false;
    } else {
      q &= b;
    }
    if (q.isEmpty()) break;
  }
  const uint64_t n_q = q.cardinality();
  const PerfSnapshot p1 = PerfSnapshot::Now();

  // 2. rowid -> PK, then dedup on PK: the rowids of an updated PK collapse
  // here, so the primary is fetched once per PK.
  std::vector<std::string> pks;
  pks.reserve(n_q);
  {
    std::vector<uint32_t> rowids(n_q);
    q.toUint32Array(rowids.data());
    const size_t batch = std::min(kMultiGetBatch, rowids.size());
    std::vector<std::string> key_storage(batch);
    std::vector<rocksdb::Slice> keys(batch);
    for (size_t off = 0; off < rowids.size(); off += kMultiGetBatch) {
      const size_t n = std::min(kMultiGetBatch, rowids.size() - off);
      for (size_t i = 0; i < n; ++i) {
        lazy_bitmaps::RowidKey(rowids[off + i], &key_storage[i]);
        keys[i] = rocksdb::Slice(key_storage[i]);
      }
      std::vector<rocksdb::PinnableSlice> values(n);
      std::vector<rocksdb::Status> statuses(n);
      db_->MultiGet(ro, cf_[kRowidMap], n, keys.data(), values.data(),
                    statuses.data());
      for (size_t i = 0; i < n; ++i)
        if (statuses[i].ok()) pks.push_back(values[i].ToString());
    }
  }
  std::sort(pks.begin(), pks.end());
  pks.erase(std::unique(pks.begin(), pks.end()), pks.end());
  const uint64_t n_pk = pks.size();
  const PerfSnapshot p2 = PerfSnapshot::Now();

  // 3. The latest record of every PK, checked against the whole CNF: bin
  // false positives and stale rowids fall out here, deleted PKs are NotFound.
  uint64_t matched = 0;
  {
    std::vector<rocksdb::Slice> keys(std::min(kMultiGetBatch, pks.size()));
    for (size_t off = 0; off < pks.size(); off += kMultiGetBatch) {
      const size_t n = std::min(kMultiGetBatch, pks.size() - off);
      for (size_t i = 0; i < n; ++i) keys[i] = rocksdb::Slice(pks[off + i]);
      std::vector<rocksdb::PinnableSlice> values(n);
      std::vector<rocksdb::Status> statuses(n);
      db_->MultiGet(ro, cf_[kPrimary], n, keys.data(), values.data(),
                    statuses.data());
      for (size_t i = 0; i < n; ++i)
        if (statuses[i].ok() &&
            query.CheckCondition(
                rocksdb::Slice(values[i].data(), values[i].size()), *layout_))
          ++matched;
    }
  }
  db_->ReleaseSnapshot(snap);

  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::high_resolution_clock::now() - start)
          .count();
  const PerfSnapshot bitmaps = p1 - p0, rowid_map = p2 - p1, index = p2 - p0;
  idx_reads_ += index.reads;
  idx_bytes_ += index.bytes;
  idx_hits_ += index.hits;
  last_scan_ = {n_q, n_pk, matched};
  std::cout << "scan (lazy-bitmaps) done: " << matched << " matched, |Q|="
            << n_q << " |PKs|=" << n_pk << ", " << elapsed
            << "ms, operands_folded=" << lazy_bitmaps::tl_operands_folded
            << ", bitmaps_read_kb=" << bitmaps.bytes / 1024
            << " rowid_map_read_kb=" << rowid_map.bytes / 1024 << "\n";
  return {static_cast<uint64_t>(elapsed), matched};
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
