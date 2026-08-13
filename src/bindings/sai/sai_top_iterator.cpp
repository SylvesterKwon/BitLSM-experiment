// Corresponds to the embedded baseline's EmbeddedIterator
// (src/bindings/embedded/embedded_top_iterator.cpp); ported to namespace
// experiment::sai. SAI-specific changes are marked below.

#include "sai_iterator.h"
#include "sai_value_codec.h"

#include <bit_lsm_query.h>

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <numeric>

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

  // 4. Arm the shadow check (port of BitLSM PR #41). The scan already reads
  // the row it is about to re-fetch, so the re-fetch is redundant whenever no
  // newer version can exist elsewhere. It is only sound for plain values: a
  // merge operand still needs merging and a blob index is a pointer, and only
  // MultiGet resolves either. The checker cannot see inside a candidate's own
  // source file, so the leaves report that part through
  // SourceHasNewerVersion().
  const bool skippable_value_format =
      cfd_->ioptions().merge_operator == nullptr &&
      !sv_->mutable_cf_options.enable_blob_files;
  if (skippable_value_format) {
    // 4-1. Whole-scan authority: with empty memtables and L0, a single
    // non-empty level (disjoint key ranges) and every file seqno-zeroed
    // (which RocksDB does only when one visible version remains), no
    // candidate can have a newer version anywhere, so batches need no
    // re-fetch at all. Writes arriving later are invisible -- the snapshot
    // seqno is already fixed.
    const VersionStorageInfo* vsi = sv_->current->storage_info();
    bool above_empty = sv_->mem->NumEntries() == 0 &&
                       sv_->imm->NumNotFlushed() == 0 &&
                       vsi->NumLevelFiles(0) == 0;
    int non_empty_level = -1;
    bool single_level = above_empty;
    for (int level = 1; single_level && level < vsi->num_non_empty_levels();
         ++level) {
      if (vsi->NumLevelFiles(level) == 0) continue;
      if (non_empty_level != -1) single_level = false;
      non_empty_level = level;
    }
    bool all_zeroed = single_level && non_empty_level != -1;
    if (all_zeroed) {
      for (const FileMetaData* f : vsi->LevelFiles(non_empty_level)) {
        if (f->fd.largest_seqno != 0) {
          all_zeroed = false;
          break;
        }
      }
    }
    authoritative_scan_ = all_zeroed;

    // 4-2. Otherwise judge candidates one by one.
    if (!authoritative_scan_) {
      scan_ctx_ = std::make_unique<bit_lsm::ScanContext>(sv_);
      checker_ = std::make_unique<bit_lsm::ShadowChecker>(*scan_ctx_,
                                                          options_.read_seqno);
      check_enabled_ = true;
    }
  }

  // 5. Create merging iterator
  smi_ = new SAIMergingIterator(sv_, options_, query_, plan_, check_enabled_);
}

SAIIterator::~SAIIterator() {
  // 0. Opt-in shadow-check report: a silent skip is indistinguishable from a
  // disabled one, so make the rate observable without touching the hot path.
  if (std::getenv("EXP_SHADOW_STATS") != nullptr) {
    std::cerr << "[sai shadow] authoritative=" << authoritative_scan_
              << " enabled=" << check_enabled_ << " checked=" << checked_keys_
              << " skipped=" << skipped_keys_ << "\n";
  }

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

  // 2. Never scan on top of a failed merge: the merging iterator stops on the
  // first child error, so anything it could still yield is a partial answer.
  if (!smi_->status().ok()) {
    status_ = smi_->status();
    return;
  }

  // 3. Try to find next valid batch which contains at least one valid data entry
  while (batch_keys_.empty() && smi_->Valid()) {
    candidate_keys_.clear();
    candidate_keys_.reserve(batch_size);
    candidate_values_.clear();
    candidate_seqnos_.clear();
    candidate_src_levels_.clear();
    candidate_src_files_.clear();
    candidate_in_file_shadowed_.clear();
    const bool keep_scan_rows = authoritative_scan_ || check_enabled_;
    if (keep_scan_rows) candidate_values_.reserve(batch_size);

    // 4. Get candidate keys, and with the skip armed the row the scan already
    // read plus the provenance needed to judge it.
    while (smi_->Valid() && candidate_keys_.size() < batch_size) {
      ParsedInternalKey ikey;
      s = rocksdb::ParseInternalKey(smi_->key(), &ikey, false);
      if (s.ok()) {
        // Compared as a Slice, so a duplicate costs a memcmp, not a copy.
        if (ikey.user_key != Slice(latest_user_key_added)) {
          candidate_keys_.emplace_back(ikey.user_key.data(),
                                       ikey.user_key.size());
          latest_user_key_added.assign(ikey.user_key.data(),
                                       ikey.user_key.size());
          if (keep_scan_rows) {
            // Copied, not borrowed: advancing smi_ can unpin the block this
            // value points into.
            Slice v = smi_->value();
            candidate_values_.emplace_back(v.data(), v.size());
          }
          if (check_enabled_) {
            candidate_seqnos_.push_back(ikey.sequence);
            candidate_src_levels_.push_back(smi_->SourceLevel());
            candidate_src_files_.push_back(smi_->SourceFileNumber());
            candidate_in_file_shadowed_.push_back(
                smi_->SourceHasNewerVersion() ? 1 : 0);
          }
        }
      }
      smi_->Next();
    }
    // The merge may have stopped on an error partway through this batch.
    // Half a batch is worse than none, so drop it and report the failure.
    if (!smi_->status().ok()) {
      status_ = smi_->status();
      candidate_keys_.clear();
      batch_keys_.clear();
      batch_values_.clear();
      return;
    }
    if (candidate_keys_.empty()) break;

    // 5. Decide which candidates still need the authoritative re-fetch. Whole
    // scan authority (see the constructor) clears every candidate at once;
    // with the check off, all of them are dirty.
    dirty_idx_.clear();
    if (authoritative_scan_) {
      // nothing to fetch
    } else if (check_enabled_) {
      for (uint32_t i = 0; i < candidate_keys_.size(); ++i) {
        // In-file shadowing first: it short-circuits the probes.
        if (candidate_in_file_shadowed_[i] ||
            checker_->MayHaveNewerVersion(
                Slice(candidate_keys_[i]), candidate_seqnos_[i],
                candidate_src_levels_[i], candidate_src_files_[i])) {
          dirty_idx_.push_back(i);
        }
      }
      checked_keys_ += candidate_keys_.size();
      skipped_keys_ += candidate_keys_.size() - dirty_idx_.size();
      // Below this skip rate the probes are pure overhead on top of a full
      // MultiGet, so stop checking for the rest of this iterator.
      constexpr uint64_t kCheckSampleKeys = 4096;
      constexpr uint64_t kMinSkipRatePct = 25;
      if (checked_keys_ >= kCheckSampleKeys &&
          skipped_keys_ * 100 < checked_keys_ * kMinSkipRatePct) {
        check_enabled_ = false;
      }
    } else {
      dirty_idx_.resize(candidate_keys_.size());
      std::iota(dirty_idx_.begin(), dirty_idx_.end(), 0);
    }

    // 6. MultiGet only the dirty subset.
    std::vector<Slice> candidate_key_slices;
    candidate_key_slices.reserve(dirty_idx_.size());
    for (uint32_t i : dirty_idx_) {
      candidate_key_slices.push_back(Slice(candidate_keys_[i]));
    }
    std::vector<PinnableSlice> pin_values(dirty_idx_.size());
    std::vector<Status> statuses(dirty_idx_.size());
    if (!dirty_idx_.empty()) {
      ReadOptions ro;
      ro.snapshot = snapshot_;
      db_->MultiGet(ro, cfh_, candidate_key_slices.size(),
                    candidate_key_slices.data(), pin_values.data(),
                    statuses.data(), true);
    }

    // 7. Ordered merge. Both branches run SAICodec::Evaluate: index mode
    // leaves the full-query post-filter to this step, so a skipped fetch must
    // still be filtered -- it is just evaluated against the row the scan read
    // instead of a re-fetched one.
    size_t d = 0;
    for (uint32_t i = 0; i < candidate_keys_.size(); ++i) {
      if (d >= dirty_idx_.size() || dirty_idx_[d] != i) {
        if (SAICodec::Evaluate(query_,
                               std::string_view(candidate_values_[i].data(),
                                                candidate_values_[i].size()),
                               options_)) {
          batch_keys_.push_back(std::move(candidate_keys_[i]));
          batch_values_.push_back(std::move(candidate_values_[i]));
        }
        continue;
      }
      const size_t j = d++;
      // 7-1. Check given candidate key exists in DB. NotFound is expected: the
      // candidate row was shadowed/deleted between the index read and this
      // fetch. Any other non-OK status is a real MultiGet failure, so this
      // batch is unverifiable -- drop it and report the failure the same way
      // a mid-batch smi_ error does above.
      if (!statuses[j].ok()) {
        if (statuses[j].IsNotFound()) continue;
        status_ = statuses[j];
        candidate_keys_.clear();
        batch_keys_.clear();
        batch_values_.clear();
        return;
      }

      // 7-2. Value validation via SAICodec::Evaluate
      if (SAICodec::Evaluate(
              query_,
              std::string_view(pin_values[j].data(), pin_values[j].size()),
              options_)) {
        // 7-3. Move key/value to validated batch if valid entry
        batch_keys_.push_back(std::move(candidate_keys_[i]));
        // Optimization: Only call ToString() for valid values
        batch_values_.push_back(pin_values[j].ToString());
      }
    }
  }

  // 7. Update valid_
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
      Status s = tc->FindTable(ReadOptions(), *cfd_->soptions(), *icmp, *meta,
                               &handle, sv_->mutable_cf_options);
      if (!s.ok()) continue;  // unreadable table contributes nothing
      auto* bbt = static_cast<BlockBasedTable*>(cache_interface.Value(handle));
      // Holds the SAI entry for this file's harvest: a block cache pin when
      // cache_index_and_filter_blocks is on (evictable after release), or an
      // unowned reference to the table-lifetime pin in Rep when off. The
      // estimates are copied out below, so it only has to outlive this
      // iteration (mirrors bit_lsm_estimator.cpp).
      CachableEntry<Block_kUserDefinedIndex> udi_entry;
      Status udi_s = bbt->GetUserDefinedIndexReader(ReadOptions(), &udi_entry);
      if (!udi_s.ok()) {
        // Planning stats degrade, never crash: the file contributes nothing
        // to the ranking. The scan itself still visits it and surfaces the
        // load failure through SAITableIterator's status.
        cache_interface.Release(handle);
        continue;
      }
      auto* idx = static_cast<SAIIndexReader*>(udi_entry.GetValue()->reader());
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
