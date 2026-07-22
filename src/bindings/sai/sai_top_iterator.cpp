// Corresponds to the embedded baseline's EmbeddedIterator
// (src/bindings/embedded/embedded_top_iterator.cpp); ported to namespace
// experiment::sai. SAI-specific changes are marked below.

#include "sai_iterator.h"
#include "sai_value_codec.h"

#include <bit_lsm_query.h>

#include <cassert>
#include <cstdint>

#include "bit_lsm_option.h"
#include "rocksdb/db.h"
#include "rocksdb/snapshot.h"
#include "table/block_based/block_based_table_reader.h"
#include "table/format.h"

namespace experiment::sai {

using namespace rocksdb;

SAIIterator::SAIIterator(DB* db, ColumnFamilyHandle* cfh,
                         bit_lsm::BitLSMOptions options,
                         bit_lsm::BitLSMQuery query, int intersection_limit)
    : db_(db),
      db_impl_(static_cast<DBImpl*>(db_)),
      cfh_(cfh),
      // 1. Create snapshot
      snapshot_(db_->GetSnapshot()),
      // For now, we only support default CF for the sake of simplicity
      cfd_(db_impl_->GetVersionSet()->GetColumnFamilySet()->GetDefault()),
      // 2. Create SuperVersion
      sv_(cfd_->GetReferencedSuperVersion(db_impl_)),
      options_(options),
      query_(query),
      intersection_limit_(intersection_limit),
      latest_user_key_added("") {
  // 3. Save snapshot's seqno to options
  options_.read_seqno = snapshot_->GetSequenceNumber();

  // Choose index predicates (Cassandra SAI conjunction heuristic).
  BuildPlan();

  // 4. Create merging iterator
  smi_ = new SAIMergingIterator(sv_, options_, query_, plan_);
}

SAIIterator::~SAIIterator() {
  // 1. Free SAIMergingIterator
  delete smi_;

  // 2. Clean up super version
  if (sv_->Unref()) {
    db_impl_->mutex()->Lock();
    sv_->Cleanup();
    db_impl_->mutex()->Unlock();
    delete sv_;
  }

  // 3. Release snapshot
  if (snapshot_ != nullptr) db_->ReleaseSnapshot(snapshot_);
}

void SAIIterator::FetchNextBatch(uint32_t batch_size) {
  Status s;

  // 1. Clean current batch
  batch_keys_.clear();
  batch_values_.clear();
  batch_cur_idx_ = 0;
  valid_ = false;

  // 2. Try to find next valid batch which contains at least one valid data entry
  while (batch_keys_.empty() && smi_->Valid()) {
    std::vector<std::string> candidate_keys;
    candidate_keys.reserve(batch_size);

    // 3. Get candidate keys
    while (smi_->Valid() && candidate_keys.size() < batch_size) {
      ParsedInternalKey ikey;
      s = rocksdb::ParseInternalKey(smi_->key(), &ikey, false);
      if (s.ok()) {
        std::string cur_user_key = ikey.user_key.ToString();
        if (cur_user_key != latest_user_key_added) {
          candidate_keys.push_back(cur_user_key);
          latest_user_key_added = cur_user_key;
        }
      }
      smi_->Next();
    }
    if (candidate_keys.empty()) break;

    // 4. MultiGet candidates from RocksDB
    std::vector<Slice> candidate_key_slices;
    candidate_key_slices.reserve(candidate_keys.size());
    for (const auto& k : candidate_keys) {
      candidate_key_slices.push_back(Slice(k));
    }
    std::vector<PinnableSlice> pin_values(candidate_keys.size());
    std::vector<Status> statuses(candidate_keys.size());
    ReadOptions ro;
    ro.snapshot = snapshot_;

    db_->MultiGet(ro, cfh_, candidate_key_slices.size(),
                  candidate_key_slices.data(), pin_values.data(),
                  statuses.data(), true);

    // 5. Cross check (replaces query_.CheckCondition with SAICodec::Evaluate)
    for (uint32_t i = 0; i < candidate_key_slices.size(); ++i) {
      // 5-1. Check given candidate key exists in DB
      if (!statuses[i].ok()) continue;

      // 5-2. Value validation via SAICodec::Evaluate
      if (SAICodec::Evaluate(
              query_,
              std::string_view(pin_values[i].data(), pin_values[i].size()),
              options_)) {
        // 5-3. Move key/value to validated batch if valid entry
        batch_keys_.push_back(std::move(candidate_keys[i]));
        // Optimization: Only call ToString() for valid values
        batch_values_.push_back(pin_values[i].ToString());
      }
    }
  }

  // 6. Update valid_
  if (!batch_keys_.empty()) valid_ = true;
}

void SAIIterator::SeekToFirst() {
  // 1. SeekToFirst internal iterator
  smi_->SeekToFirst();
  latest_user_key_added.clear();

  // 2. Prepare next batch
  FetchNextBatch(1024);
}

void SAIIterator::Next() {
  assert(Valid());

  // 1. Forward batch cursor
  batch_cur_idx_++;

  // 2. If all entries in batch consumed, prepare next batch
  if (batch_cur_idx_ >= batch_keys_.size()) {
    FetchNextBatch(1024);
  }
}

Slice SAIIterator::key() const {
  assert(Valid());
  return batch_keys_[batch_cur_idx_];
}

Slice SAIIterator::value() const {
  assert(Valid());
  return batch_values_[batch_cur_idx_];
}

// Conjunction heuristic. Corresponds to Cassandra SAI:
//  - per-predicate cardinality = sum of per-SSTable posting counts
//    (KeyRangeUnionIterator count aggregation, KeyRangeUnionIterator.java:202)
//  - keep only the `limit` most selective for intersection
//    (KeyRangeIntersectionIterator.java:290-315; default limit 2 =
//    cassandra.sai.intersection_clause_limit)
// Deviations D3 (memtable not in ranking) and D5 (counts read from inline
// metadata; losers never open postings) — design doc §8.
void SAIIterator::BuildPlan() {
  std::vector<SAIFact> facts = ExtractFacts(query_, options_);
  if (facts.empty()) {
    plan_.full_scan = true;
    return;
  }
  // Ranking pass: walk every live SSTable's SAIIndexReader and sum estimates.
  TableCache* tc = cfd_->table_cache();
  const VersionStorageInfo* vsi = sv_->current->storage_info();
  const InternalKeyComparator* icmp = vsi->InternalComparator();
  TableCache::CacheInterface cache_interface = tc->get_cache();
  for (int level = 0; level < vsi->num_non_empty_levels(); ++level) {
    for (FileMetaData* meta : vsi->LevelFiles(level)) {
      TableCache::TypedHandle* handle = nullptr;
      Status s = tc->FindTable(ReadOptions(), FileOptions(), *icmp, *meta,
                               &handle, sv_->mutable_cf_options);
      if (!s.ok()) continue;  // unreadable table contributes nothing
      auto* bbt = static_cast<BlockBasedTable*>(cache_interface.Value(handle));
      auto* idx = static_cast<SAIIndexReader*>(
          bbt->get_rep()->index_reader.get()->GetUDIReader());
      for (auto& f : facts) f.est += idx->Estimate(f);
      cache_interface.Release(handle);
    }
  }
  const std::vector<uint32_t> keep = ChooseTopK(facts, intersection_limit_);
  plan_.chosen.clear();
  for (uint32_t k : keep) plan_.chosen.push_back(facts[k]);
  plan_.full_scan = false;
}

}  // namespace experiment::sai
