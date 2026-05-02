#include "si_eager_binding.h"
#include <algorithm>
#include <chrono>
#include <cxxopts.hpp>

using namespace std;
using namespace rocksdb;
using namespace bit_lsm;

namespace experiment {

void SIEagerBinding::InsertSIValue(vector<Slice>* si_value,
                                    const Slice& key) {
  auto it = lower_bound(
      si_value->begin(), si_value->end(), key,
      [](const Slice& a, const Slice& b) { return a.compare(b) < 0; });
  if (it != si_value->end() && key.compare(*it) == 0) return;
  size_t pos = distance(si_value->begin(), it);
  si_value->push_back(Slice());
  Slice* data = si_value->data();
  size_t move_count = si_value->size() - 1 - pos;
  if (move_count > 0)
    memmove(&data[pos + 1], &data[pos], move_count * sizeof(Slice));
  data[pos] = key;
}

void SIEagerBinding::Open(int argc, char* argv[], const string& db_path,
                           const BitLSMOptions& opts) {
  options_ = opts;

  cxxopts::Options cxx("si-eager", "");
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

  db_ = benchmark::OpenSITransactionDB(db_path, {}, max_bg_jobs, stats_);
}

void SIEagerBinding::Put(const string& pk, const vector<Attr>& attrs,
                          const string& payload) {
  thread_local string serialized_value;
  thread_local vector<Slice> si_value_vec;
  thread_local string encoded_si_value;
  thread_local string existing_si_str;
  EncodeValue(options_, attrs, payload, serialized_value);
  Transaction* txn = db_.txn_db->BeginTransaction(wo_);

  ReadOptions ro;
  for (uint32_t attr_idx = 0; attr_idx < options_.attr_num; ++attr_idx) {
    string si_key;
    if (options_.attr_types[attr_idx] == AttrType::CATEGORICAL) {
      const string& sk_value = get<string>(attrs[attr_idx]);
      si_key = GetInternalSIKey(attr_idx, sk_value);
    } else {
      double sk_value = get<double>(attrs[attr_idx]);
      si_key = GetInternalSIKey(attr_idx, sk_value);
    }

    existing_si_str.clear();
    auto s = txn->Get(ro, db_.cf_handles[1], si_key, &existing_si_str);

    if (s.IsNotFound()) {
      si_value_vec.clear();
      si_value_vec.push_back(Slice(pk));
      encoded_si_value.clear();
      EncodeIndexValue(&si_value_vec, &encoded_si_value);
      txn->Put(db_.cf_handles[1], si_key, encoded_si_value);
    } else {
      si_value_vec.clear();
      Slice si_slice(existing_si_str);
      DecodeIndexValue(si_slice, &si_value_vec);
      InsertSIValue(&si_value_vec, Slice(pk));
      encoded_si_value.clear();
      EncodeIndexValue(&si_value_vec, &encoded_si_value);
      txn->Put(db_.cf_handles[1], si_key, encoded_si_value);
    }
  }

  txn->Put(db_.cf_handles[0], pk, serialized_value);
  txn->Commit();
  delete txn;
}

ScanResult SIEagerBinding::Scan(BitLSMQuery& query) {
  auto plan = benchmark::MapQueryToSILookups(query, options_);

  if (plan.si_lookups.empty()) {
    auto r = benchmark::ScanFullTable(db_.txn_db, db_.cf_handles, query,
                                       options_, 0);
    return {r.time_elapsed_ms, r.records_matched};
  }

  auto GetPKList = [this](const benchmark::SILookup& lookup) -> vector<string> {
    if (lookup.type == benchmark::SILookupType::kPointLookup) {
      ReadOptions ro;
      vector<string> result;
      for (const auto& val : lookup.sk_values) {
        string si_value_str;
        auto s = db_.txn_db->Get(ro, db_.cf_handles[1],
                                  GetInternalSIKey(lookup.attr_idx, val),
                                  &si_value_str);
        if (s.IsNotFound()) continue;
        vector<Slice> si_value;
        Slice si_slice(si_value_str);
        DecodeIndexValue(si_slice, &si_value);
        for (auto& pk : si_value)
          result.push_back(pk.ToString());
      }
      std::sort(result.begin(), result.end());
      return result;
    } else {
      ReadOptions ro;
      auto* iter = db_.txn_db->NewIterator(ro, db_.cf_handles[1]);

      string seek_key;
      if (lookup.lower_bound.has_value()) {
        seek_key = GetInternalSIKey(lookup.attr_idx, *lookup.lower_bound);
      } else {
        seek_key = GetInternalSIKey(lookup.attr_idx, Slice(""));
      }

      string prefix = std::to_string(lookup.attr_idx);
      prefix.resize(4, ' ');

      vector<string> all_pks;
      for (iter->Seek(seek_key); iter->Valid(); iter->Next()) {
        auto key = iter->key();
        if (key.size() < 4 + 8 ||
            key.ToStringView().substr(0, 4) != prefix)
          break;

        double key_val = DecodeDoubleOrderPreserving(key.data() + 4);

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

        vector<Slice> si_value;
        Slice val_slice = iter->value();
        DecodeIndexValue(val_slice, &si_value);
        for (auto& pk_slice : si_value)
          all_pks.push_back(pk_slice.ToString());
      }
      delete iter;

      std::sort(all_pks.begin(), all_pks.end());
      return all_pks;
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

WriteStats SIEagerBinding::GetWriteStats() {
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

void SIEagerBinding::Close() { benchmark::CloseSITransactionDB(db_); }

string SIEagerBinding::ParamSuffix() const {
  return "_strategy_" + benchmark::SIStrategyToString(strategy_);
}

}  // namespace experiment
