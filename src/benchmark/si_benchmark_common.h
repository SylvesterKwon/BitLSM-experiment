#pragma once

#include "benchmark_experiment.h"
#include "rocksdb_common_option.h"
#include "si_index_utils.h"
#include <algorithm>
#include <optional>
#include <rocksdb/filter_policy.h>
#include <rocksdb/merge_operator.h>
#include <rocksdb/statistics.h>
#include <rocksdb/table.h>
#include <rocksdb/utilities/transaction_db.h>
#include <unordered_map>

namespace benchmark {

// ---------- Multi-attribute query strategy ----------

enum class SIStrategy { kIndexMerge, kPostFiltering };

inline SIStrategy ParseSIStrategy(const std::string& s) {
  if (s == "pf" || s == "post_filter")
    return SIStrategy::kPostFiltering;
  return SIStrategy::kIndexMerge;
}

inline std::string SIStrategyToString(SIStrategy s) {
  return s == SIStrategy::kPostFiltering ? "pf" : "im";
}

enum class SILookupType { kPointLookup, kRangeScan };

struct SILookup {
  uint32_t attr_idx;
  SILookupType type;
  bit_lsm::IndexType attr_type;

  // For kPointLookup (UNORDERED + EQUAL):
  // Multiple values represent OR within a clause (e.g., attr=1 OR attr=3).
  std::vector<std::string> sk_values;

  // For kRangeScan (ORDERED + range ops):
  std::optional<double> lower_bound;
  bool lower_inclusive = true; // >= vs >
  std::optional<double> upper_bound;
  bool upper_inclusive = false; // <= vs <
};

struct SIQueryPlan {
  std::vector<SILookup> si_lookups;
  bit_lsm::BitLSMQuery post_filter_query; // remaining conditions
};

inline SIQueryPlan MapQueryToSILookups(const bit_lsm::BitLSMQuery& query,
                                       const bit_lsm::BitLSMOptions& options) {
  SIQueryPlan plan;

  // Group ORDERED conditions by attr_idx to coalesce bounds.
  // Reserve a slot in si_lookups on first encounter so that clause_groups
  // ordering (e.g. hint-based reorder) is preserved.
  std::unordered_map<uint32_t, size_t> range_slot; // attr_idx → si_lookups index

  // Process each clause (AND between clauses, OR within a clause).
  for (const auto& clause : query.clause_groups) {
    // Check if this clause is a pure categorical OR on a single attr
    // (all conditions are EQUAL on the same categorical attr).
    bool pure_cat_or = !clause.empty();
    uint32_t cat_attr = clause.empty() ? 0 : clause[0].attr_idx;
    for (const auto& cond : clause) {
      if (cond.attr_idx != cat_attr ||
          cond.op != bit_lsm::CompareOp::EQUAL ||
          options.attr_specs[cond.attr_idx].index_type != bit_lsm::IndexType::kEquality) {
        pure_cat_or = false;
        break;
      }
    }

    if (pure_cat_or) {
      // Single multi-value point lookup (OR union handled in GetPKList)
      SILookup lk;
      lk.attr_idx = cat_attr;
      lk.type = SILookupType::kPointLookup;
      lk.attr_type = bit_lsm::IndexType::kEquality;
      for (const auto& cond : clause)
        lk.sk_values.push_back(std::get<std::string>(cond.value));
      plan.si_lookups.push_back(std::move(lk));
    } else if (clause.size() == 1) {
      // Single-condition clause (common case: AND-only queries)
      const auto& cond = clause[0];
      if (options.attr_specs[cond.attr_idx].index_type == bit_lsm::IndexType::kRange) {
        // Reserve a slot on first encounter to preserve clause ordering
        auto [it, inserted] = range_slot.emplace(cond.attr_idx,
                                                  plan.si_lookups.size());
        if (inserted) {
          SILookup lk;
          lk.attr_idx = cond.attr_idx;
          lk.type = SILookupType::kRangeScan;
          lk.attr_type = bit_lsm::IndexType::kRange;
          plan.si_lookups.push_back(std::move(lk));
        }
        auto& lk = plan.si_lookups[it->second];
        double val = std::get<double>(cond.value);
        switch (cond.op) {
        case bit_lsm::CompareOp::GREATER_EQUAL:
          lk.lower_bound = val; lk.lower_inclusive = true; break;
        case bit_lsm::CompareOp::GREATER:
          lk.lower_bound = val; lk.lower_inclusive = false; break;
        case bit_lsm::CompareOp::LESS_EQUAL:
          lk.upper_bound = val; lk.upper_inclusive = true; break;
        case bit_lsm::CompareOp::LESS:
          lk.upper_bound = val; lk.upper_inclusive = false; break;
        case bit_lsm::CompareOp::EQUAL:
          lk.lower_bound = val; lk.lower_inclusive = true;
          lk.upper_bound = val; lk.upper_inclusive = true; break;
        }
      } else if (cond.op == bit_lsm::CompareOp::EQUAL) {
        SILookup lk;
        lk.attr_idx = cond.attr_idx;
        lk.type = SILookupType::kPointLookup;
        lk.attr_type = bit_lsm::IndexType::kEquality;
        lk.sk_values.push_back(std::get<std::string>(cond.value));
        plan.si_lookups.push_back(std::move(lk));
      } else {
        plan.post_filter_query.clause_groups.push_back(clause);
      }
    } else {
      // Mixed clause (different attrs or non-EQUAL ops) → post-filter
      plan.post_filter_query.clause_groups.push_back(clause);
    }
  } // end for (clause)

  return plan;
}

// ---------- Merge operator for lazy updates (LU) ----------

class SIValueMergeOperator : public rocksdb::MergeOperator {
public:
  const char* Name() const override { return "SIValueMergeOperator"; }

  bool FullMergeV2(const MergeOperationInput& merge_in,
                   MergeOperationOutput* merge_out) const override {
    std::vector<rocksdb::Slice> all_operand_list = merge_in.operand_list;
    if (merge_in.existing_value)
      all_operand_list.push_back(*merge_in.existing_value);
    MergeIndexValue(all_operand_list, &merge_out->new_value);
    return true;
  }

  bool PartialMergeMulti(const rocksdb::Slice&,
                         const std::deque<rocksdb::Slice>& operand_list,
                         std::string* new_value,
                         rocksdb::Logger*) const override {
    std::vector<rocksdb::Slice> all(operand_list.begin(), operand_list.end());
    MergeIndexValue(all, new_value);
    return true;
  }
};

// ---------- Common TransactionDB open ----------

struct SIDBHandles {
  rocksdb::TransactionDB* txn_db = nullptr;
  std::vector<rocksdb::ColumnFamilyHandle*> cf_handles;
};

inline SIDBHandles OpenSITransactionDB(
    const std::string& db_path,
    rocksdb::ColumnFamilyOptions si_cf_opts_override = {},
    int max_background_jobs = 6,
    std::shared_ptr<rocksdb::Statistics> statistics = nullptr,
    std::optional<rocksdb::BlockBasedTableOptions> si_table_override = {}) {
  SIDBHandles h;

  rocksdb::Options opts;
  opts.create_if_missing = true;
  opts.create_missing_column_families = true;
  opts.max_background_jobs = max_background_jobs;
  opts.bytes_per_sync = 1048576;
  opts.compaction_pri = rocksdb::kMinOverlappingRatio;
  opts.max_write_buffer_number = 5;
  if (statistics)
    opts.statistics = statistics;
  experiment::ApplyRocksdbCommonOptions(opts);

  rocksdb::BlockBasedTableOptions table_options;
  experiment::ApplyRocksdbCommonTableOptions(table_options);
  opts.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));

  rocksdb::ColumnFamilyOptions primary_cf_opts(opts);
  primary_cf_opts.level_compaction_dynamic_level_bytes = true;
  // Record-CF Bloom on the primary CF only; the secondary CF keeps its own
  // filter (si-ck prefix Bloom via override) or none (si-lu/eager).
  rocksdb::BlockBasedTableOptions primary_table_options;
  experiment::ApplyRocksdbCommonTableOptions(primary_table_options);
  experiment::ApplyRecordCfBloom(primary_table_options);
  primary_cf_opts.table_factory.reset(
      rocksdb::NewBlockBasedTableFactory(primary_table_options));

  // Merge SI CF options with base
  rocksdb::ColumnFamilyOptions si_cf_opts(opts);
  si_cf_opts.level_compaction_dynamic_level_bytes = true;
  if (si_cf_opts_override.merge_operator)
    si_cf_opts.merge_operator = si_cf_opts_override.merge_operator;
  if (si_cf_opts_override.prefix_extractor)
    si_cf_opts.prefix_extractor = si_cf_opts_override.prefix_extractor;
  // Secondary factory: the caller's distinguishing bits (si-ck sets
  // whole_key_filtering + a prefix Bloom) plus the shared table regime. Without
  // an override the secondary CF inherits the common base factory.
  if (si_table_override) {
    experiment::ApplyRocksdbCommonTableOptions(*si_table_override);
    si_cf_opts.table_factory.reset(
        rocksdb::NewBlockBasedTableFactory(*si_table_override));
  }

  std::vector<rocksdb::ColumnFamilyDescriptor> column_families = {
      {rocksdb::kDefaultColumnFamilyName, primary_cf_opts},
      {"standalone_index", si_cf_opts},
  };

  rocksdb::TransactionDBOptions txn_db_opts;
  auto s = rocksdb::TransactionDB::Open(
      opts, txn_db_opts, db_path, column_families, &h.cf_handles, &h.txn_db);
  if (!s.ok()) {
    std::cerr << "Failed to open TransactionDB: " << s.ToString() << "\n";
    exit(1);
  }
  return h;
}

inline void CloseSITransactionDB(SIDBHandles& h) {
  if (!h.txn_db)
    return;
  for (auto* handle : h.cf_handles)
    h.txn_db->DestroyColumnFamilyHandle(handle);
  h.cf_handles.clear();
  rocksdb::WaitForCompactOptions wait_opts;
  wait_opts.close_db = true;
  h.txn_db->WaitForCompact(wait_opts);
  delete h.txn_db;
  h.txn_db = nullptr;
  std::cout << "DB successfully closed\n";
}

// ---------- Common Scan helpers ----------

// IndexMerge: lookup each SI, intersect PK lists, MultiGet, post-filter
inline ReadResult ScanByIndexMerge(
    rocksdb::TransactionDB* txn_db,
    const std::vector<rocksdb::ColumnFamilyHandle*>& cf_handles,
    SIQueryPlan& plan, const bit_lsm::BitLSMOptions& options, uint64_t n,
    std::function<std::vector<std::string>(const SILookup&)> GetPKList) {
  auto start = std::chrono::high_resolution_clock::now();
  // One layout for the whole scan: CheckCondition takes a cached layout, and
  // building one per candidate row costs four vector allocations.
  const bit_lsm::ValueLayout layout(options);

  bool is_first = true;
  std::vector<std::string> merged;
  for (auto& lookup : plan.si_lookups) {
    auto pk_list = GetPKList(lookup);
    if (is_first) {
      merged = std::move(pk_list);
      is_first = false;
    } else {
      std::vector<std::string> tmp;
      std::set_intersection(merged.begin(), merged.end(), pk_list.begin(),
                            pk_list.end(), std::back_inserter(tmp));
      merged = std::move(tmp);
    }
    if (merged.empty())
      break;
  }

  // MultiGet from primary CF (batched)
  uint64_t matched = 0;
  if (!merged.empty()) {
    static constexpr size_t kBatchSize = 1000000;
    rocksdb::ReadOptions ro;

    for (size_t offset = 0; offset < merged.size(); offset += kBatchSize) {
      size_t batch_len = std::min(kBatchSize, merged.size() - offset);

      std::vector<rocksdb::Slice> keys(batch_len);
      for (size_t i = 0; i < batch_len; ++i)
        keys[i] = rocksdb::Slice(merged[offset + i]);

      std::vector<rocksdb::PinnableSlice> values(batch_len);
      std::vector<rocksdb::Status> statuses(batch_len);
      txn_db->MultiGet(ro, cf_handles[0], batch_len, keys.data(),
                       values.data(), statuses.data());

      for (size_t i = 0; i < batch_len; ++i) {
        if (statuses[i].ok()) {
          if (plan.post_filter_query.clause_groups.empty() ||
              plan.post_filter_query.CheckCondition(
                  rocksdb::Slice(values[i].data(), values[i].size()), layout))
            matched++;
        }
      }
    }
  }

  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::high_resolution_clock::now() - start)
                     .count();
  std::cout << "scan (im) done: " << matched << "/" << n
            << " matched, " << elapsed << "ms\n";
  return {static_cast<uint64_t>(elapsed), matched,
          n > 0 ? static_cast<double>(matched) / n : 0.0};
}

// PostFiltering: lookup first SI, MultiGet, filter all remaining conditions
inline ReadResult ScanByPostFiltering(
    rocksdb::TransactionDB* txn_db,
    const std::vector<rocksdb::ColumnFamilyHandle*>& cf_handles,
    SIQueryPlan& plan, const bit_lsm::BitLSMQuery& original_query,
    const bit_lsm::BitLSMOptions& options, uint64_t n,
    std::function<std::vector<std::string>(const SILookup&)> GetPKList) {
  auto start = std::chrono::high_resolution_clock::now();
  // One layout for the whole scan; see ScanByIndexMerge.
  const bit_lsm::ValueLayout layout(options);

  // Use first SI lookup only (caller may reorder clause_groups via
  // most_selective_attr hint so that the most selective attr lands here).
  auto pk_list = GetPKList(plan.si_lookups[0]);

  // Build filter query: all conditions except those for the first SI lookup's
  // attr
  uint32_t first_attr = plan.si_lookups[0].attr_idx;
  bit_lsm::BitLSMQuery filter_query;
  for (const auto& clause : original_query.clause_groups) {
    // Keep clauses that don't solely target the first SI lookup's attr
    bool dominated = true;
    for (const auto& cond : clause) {
      if (cond.attr_idx != first_attr) {
        dominated = false;
        break;
      }
    }
    if (!dominated)
      filter_query.clause_groups.push_back(clause);
  }

  uint64_t matched = 0;
  if (!pk_list.empty()) {
    static constexpr size_t kBatchSize = 1000000;
    rocksdb::ReadOptions ro;

    for (size_t offset = 0; offset < pk_list.size(); offset += kBatchSize) {
      size_t batch_len = std::min(kBatchSize, pk_list.size() - offset);

      std::vector<rocksdb::Slice> keys(batch_len);
      for (size_t i = 0; i < batch_len; ++i)
        keys[i] = rocksdb::Slice(pk_list[offset + i]);

      std::vector<rocksdb::PinnableSlice> values(batch_len);
      std::vector<rocksdb::Status> statuses(batch_len);
      txn_db->MultiGet(ro, cf_handles[0], batch_len, keys.data(),
                       values.data(), statuses.data());

      for (size_t i = 0; i < batch_len; ++i) {
        if (statuses[i].ok()) {
          if (filter_query.clause_groups.empty() ||
              filter_query.CheckCondition(
                  rocksdb::Slice(values[i].data(), values[i].size()), layout))
            matched++;
        }
      }
    }
  }

  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::high_resolution_clock::now() - start)
                     .count();
  std::cout << "scan (pf) done: " << matched << "/" << n
            << " matched, " << elapsed << "ms\n";
  return {static_cast<uint64_t>(elapsed), matched,
          n > 0 ? static_cast<double>(matched) / n : 0.0};
}

// Full table scan fallback
inline ReadResult
ScanFullTable(rocksdb::TransactionDB* txn_db,
              const std::vector<rocksdb::ColumnFamilyHandle*>& cf_handles,
              bit_lsm::BitLSMQuery& query,
              const bit_lsm::BitLSMOptions& options, uint64_t n) {
  rocksdb::ReadOptions ro;
  auto* it = txn_db->NewIterator(ro, cf_handles[0]);
  uint64_t matched = 0, total = 0;
  // One layout for the whole scan; see ScanByIndexMerge.
  const bit_lsm::ValueLayout layout(options);
  auto start = std::chrono::high_resolution_clock::now();
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    total++;
    if (query.CheckCondition(it->value(), layout))
      matched++;
  }
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::high_resolution_clock::now() - start)
                     .count();
  delete it;
  std::cout << "scan (full_table) done: " << matched << "/" << total
            << " matched, " << elapsed << "ms\n";
  return {static_cast<uint64_t>(elapsed), matched,
          total > 0 ? static_cast<double>(matched) / total : 0.0};
}

} // namespace benchmark
