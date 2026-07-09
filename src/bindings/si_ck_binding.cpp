#include "si_ck_binding.h"
#include <algorithm>
#include <chrono>
#include <cxxopts.hpp>
#include <rocksdb/filter_policy.h>
#include <rocksdb/slice_transform.h>

using namespace std;
using namespace rocksdb;
using namespace bit_lsm;

namespace experiment {

void SICKBinding::Open(int argc, char* argv[], const string& db_path,
                        const BitLSMOptions& opts) {
  options_ = opts;

  cxxopts::Options cxx("si-ck", "");
  cxx.allow_unrecognised_options();
  cxx.add_options()("read_strategy", "im or pf",
                    cxxopts::value<string>()->default_value("im"))
                   ("max_background_jobs", "",
                    cxxopts::value<int>()->default_value("6"))
                   ("exp_type", "",
                    cxxopts::value<string>()->default_value("write_seq"));
  auto result = cxx.parse(argc, argv);
  strategy_ =
      benchmark::ParseSIStrategy(result["read_strategy"].as<string>());
  int max_bg_jobs = result["max_background_jobs"].as<int>();
  bool wa_mode = (result["exp_type"].as<string>() == "write_seq_wa");
  if (wa_mode) stats_ = rocksdb::CreateDBStatistics();

  ColumnFamilyOptions si_cf_opts;
  BlockBasedTableOptions si_table_options;
  si_table_options.filter_policy.reset(NewBloomFilterPolicy(10, false));
  si_table_options.whole_key_filtering = false;
  si_cf_opts.table_factory.reset(
      NewBlockBasedTableFactory(si_table_options));
  si_cf_opts.prefix_extractor.reset(
      NewCappedPrefixTransform(idx_no_prefix_size_ + si_prefix_length_));

  db_ = benchmark::OpenSITransactionDB(db_path, si_cf_opts, max_bg_jobs, stats_);
}

void SICKBinding::Put(const string& pk, const vector<Attr>& attrs,
                       const string& payload) {
  thread_local string serialized_value;
  thread_local string si_key_buf;
  EncodeValue(options_, attrs, payload, serialized_value);
  Transaction* txn = db_.txn_db->BeginTransaction(wo_);

  for (uint32_t attr_idx = 0; attr_idx < options_.attr_num; ++attr_idx) {
    if (options_.attr_specs[attr_idx].role == AttrRole::UNORDERED) {
      const string& sk_value = get<string>(attrs[attr_idx]);
      si_key_buf = sk_value;
      si_key_buf.resize(si_prefix_length_, ' ');
      si_key_buf += pk;
      txn->Put(db_.cf_handles[1],
               GetInternalSIKey(attr_idx, si_key_buf, idx_no_prefix_size_),
               "");
    } else {
      double sk_value = get<double>(attrs[attr_idx]);
      char encoded[8];
      EncodeDoubleOrderPreserving(sk_value, encoded);
      si_key_buf.assign(encoded, 8);
      si_key_buf.resize(si_prefix_length_, '\0');
      si_key_buf += pk;
      txn->Put(db_.cf_handles[1],
               GetInternalSIKey(attr_idx, si_key_buf, idx_no_prefix_size_),
               "");
    }
  }

  txn->Put(db_.cf_handles[0], pk, serialized_value);
  txn->Commit();
  delete txn;
}

ScanResult SICKBinding::Scan(BitLSMQuery& query) {
  auto plan = benchmark::MapQueryToSILookups(query, options_);

  if (plan.si_lookups.empty()) {
    auto r = benchmark::ScanFullTable(db_.txn_db, db_.cf_handles, query,
                                       options_, 0);
    return {r.time_elapsed_ms, r.records_matched};
  }

  auto GetPKList = [this](const benchmark::SILookup& lookup) -> vector<string> {
    if (lookup.type == benchmark::SILookupType::kPointLookup) {
      ReadOptions si_ro;
      si_ro.auto_prefix_mode = true;
      si_ro.prefix_same_as_start = true;
      si_ro.total_order_seek = false;
      uint32_t prefix_size = idx_no_prefix_size_ + si_prefix_length_;

      vector<string> pk_list;
      for (const auto& val : lookup.sk_values) {
        string resized_sk = val;
        resized_sk.resize(si_prefix_length_, ' ');
        string seek_key = GetInternalSIKey(lookup.attr_idx, resized_sk);
        auto* it = db_.txn_db->NewIterator(si_ro, db_.cf_handles[1]);
        for (it->Seek(seek_key); it->Valid(); it->Next()) {
          auto key = it->key();
          pk_list.push_back(
              key.ToString().substr(prefix_size, key.size() - prefix_size));
        }
        delete it;
      }
      std::sort(pk_list.begin(), pk_list.end());
      return pk_list;
    } else {
      ReadOptions si_ro;
      si_ro.total_order_seek = true;

      string seek_key;
      if (lookup.lower_bound.has_value()) {
        char encoded[8];
        EncodeDoubleOrderPreserving(*lookup.lower_bound, encoded);
        string sk_part(encoded, 8);
        sk_part.resize(si_prefix_length_, '\0');
        seek_key = GetInternalSIKey(lookup.attr_idx, sk_part);
      } else {
        string sk_part(si_prefix_length_, '\0');
        seek_key = GetInternalSIKey(lookup.attr_idx, sk_part);
      }

      string prefix = std::to_string(lookup.attr_idx);
      prefix.resize(idx_no_prefix_size_, ' ');

      uint32_t prefix_size = idx_no_prefix_size_ + si_prefix_length_;

      auto* it = db_.txn_db->NewIterator(si_ro, db_.cf_handles[1]);
      vector<string> pk_list;
      for (it->Seek(seek_key); it->Valid(); it->Next()) {
        auto key = it->key();
        if (key.size() < prefix_size ||
            key.ToStringView().substr(0, idx_no_prefix_size_) != prefix)
          break;

        double key_val =
            DecodeDoubleOrderPreserving(key.data() + idx_no_prefix_size_);

        if (lookup.lower_bound.has_value()) {
          if (lookup.lower_inclusive) {
            if (key_val < *lookup.lower_bound) continue;
          } else {
            if (key_val <= *lookup.lower_bound) continue;
          }
        }

        if (lookup.upper_bound.has_value()) {
          if (lookup.upper_inclusive) {
            if (key_val > *lookup.upper_bound) break;
          } else {
            if (key_val >= *lookup.upper_bound) break;
          }
        }

        pk_list.push_back(
            key.ToString().substr(prefix_size, key.size() - prefix_size));
      }
      delete it;
      std::sort(pk_list.begin(), pk_list.end());
      return pk_list;
    }
  };

  benchmark::ReadResult r;
  if (strategy_ == benchmark::SIStrategy::kIndexMerge) {
    r = benchmark::ScanByIndexMerge(db_.txn_db, db_.cf_handles, plan,
                                     options_, 0, GetPKList);
  } else {
    r = benchmark::ScanByPostFiltering(db_.txn_db, db_.cf_handles, plan,
                                        query, options_, 0, GetPKList);
  }
  return {r.time_elapsed_ms, r.records_matched};
}

WriteStats SICKBinding::GetWriteStats() {
  if (!db_.txn_db || !stats_) return {};
  WaitForCompactOptions wfco;
  wfco.flush = true;
  wfco.wait_for_purge = true;
  db_.txn_db->WaitForCompact(wfco);
  CompactRangeOptions cro;
  cro.bottommost_level_compaction = BottommostLevelCompaction::kForce;
  cro.exclusive_manual_compaction = true;
  for (auto* h : db_.cf_handles)
    db_.txn_db->CompactRange(cro, h, nullptr, nullptr);
  db_.txn_db->WaitForCompact(wfco);
  return {stats_->getTickerCount(rocksdb::Tickers::FLUSH_WRITE_BYTES),
          stats_->getTickerCount(rocksdb::Tickers::COMPACT_WRITE_BYTES)};
}

void SICKBinding::Close() { benchmark::CloseSITransactionDB(db_); }

string SICKBinding::ParamSuffix() const {
  return "_strategy_" + benchmark::SIStrategyToString(strategy_);
}

}  // namespace experiment
