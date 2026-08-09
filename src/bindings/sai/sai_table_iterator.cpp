// Per-SST iterator for the SAI baseline.
// Full-scan mode: port of EmbeddedTableIterator's block scan
// (src/bindings/embedded/embedded_table_iterator.cpp), candidate set = ALL
// blocks, exact SAICodec::Evaluate per entry.
// Index mode: posting intersection -> candidate rowIds -> (block,
// ordinal) fetch; corresponds to Cassandra SAI's index search producing
// candidate primary keys (disk/v1/V1SSTableIndex.search + PostingListRangeIterator).
#include <bit_lsm_query.h>
#include <cstdint>
#include <limits>
#include "rocksdb/options.h"
#include "table/block_based/block.h"
#include "table/format.h"
#define TEST_CACHE_LINE_SIZE \
  64  // matches embedded_table_iterator.cpp / sabi_table_iterator.cpp

#include <iostream>

#include "sai_iterator.h"
#include "sai_value_codec.h"
#include "table/block_based/block_based_table_reader.h"
#include "table/block_based/block_based_table_reader_impl.h"
#include "util/coding.h"

namespace experiment::sai {
using namespace rocksdb;

SAITableIterator::SAITableIterator(BlockBasedTable* bbt,
                                   bit_lsm::BitLSMOptions options,
                                   bit_lsm::BitLSMQuery query, SAIPlan plan)
    : options_(std::move(options)),
      bbt_(bbt),
      query_(std::move(query)),
      plan_(std::move(plan)),
      block_prefetcher_(
          /*compaction_readahead_size=*/0,
          bbt->get_rep()->table_options.initial_auto_readahead_size) {
  // Holds the SAI entry for this iterator's lifetime: a block cache pin when
  // cache_index_and_filter_blocks is on (evictable after release), or an
  // unowned reference to the table-lifetime pin in Rep when off. Mirrors
  // SABITableIterator (third_party/BitLSM/src/include/sabi_table_iterator.cpp).
  Status s = bbt_->GetUserDefinedIndexReader(ReadOptions(), &udi_entry_);
  if (!s.ok()) {
    // Every failure here is fatal for the scan, NotFound (an SST carrying no
    // SAI block) included: the query path has no fallback scan, so skipping
    // this file would silently drop every row it holds. Record the status so
    // the parent iterators stop instead of reading it as "no matching rows".
    std::cerr << "Failed to load SAI index: " << s.ToString() << "\n";
    status_ = s;
    return;  // stays !Valid(); SeekToFirst is a no-op
  }
  idx_ = static_cast<SAIIndexReader*>(udi_entry_.GetValue()->reader());

  if (!plan_.full_scan) BuildCursor();
}

void SAITableIterator::BuildCursor() {
  // Corresponds to Cassandra's per-SSTable index search: one posting iterator
  // per chosen predicate, intersected (disk/v1/V1SSTableIndex.search ->
  // KeyRangeIntersectionIterator). Any predicate empty in this SSTable ->
  // the whole SSTable yields no candidates (disjoint short-circuit,
  // KeyRangeIntersectionIterator.buildIterator:322-331).
  std::vector<std::unique_ptr<RowCursor>> children;
  for (const SAIFact& f : plan_.chosen) {
    auto c = idx_->OpenCursor(f);
    if (!c || !c->Valid()) {
      cursor_.reset();
      return;
    }
    children.push_back(std::move(c));
  }
  if (children.empty()) {  // defensive: BuildPlan guarantees chosen non-empty
    cursor_.reset();
    return;
  }
  if (children.size() == 1) {
    cursor_ = std::move(children[0]);  // single predicate: no intersection
  } else {
    cursor_ = std::make_unique<IntersectionCursor>(std::move(children));
  }
}

void SAITableIterator::LoadNextBlockScan() {
  const uint32_t B = static_cast<uint32_t>(idx_->block_handles.size());
  while (true) {
    ++cur_block_idx_;
    if (cur_block_idx_ >= static_cast<int32_t>(B)) {
      valid_ = false;
      return;
    }
    keys_buf_.clear();
    values_buf_.clear();
    Status s;
    BlockHandle bh;
    bh.set_offset(idx_->block_handles[cur_block_idx_].offset);
    bh.set_size(idx_->block_handles[cur_block_idx_].size);
    // Standard iterator readahead (implicit auto mode: ReadOptions default
    // readahead_size == 0). The prefetcher only allocates its buffer after
    // enough sequential reads, so this call is cheap for sparse targets.
    ReadOptions read_options;
    block_prefetcher_.PrefetchIfNeeded(
        bbt_->get_rep(), bh, read_options.readahead_size,
        /*is_for_compaction=*/false, /*no_sequential_checking=*/false,
        read_options, /*readaheadsize_cb=*/nullptr,
        /*is_async_io_prefetch=*/false);
    DataBlockIter* new_biter = bbt_->NewDataBlockIterator<DataBlockIter>(
        read_options, bh, nullptr, BlockType::kData, nullptr, nullptr,
        block_prefetcher_.prefetch_buffer(), false, false, s, true);
    biter_.reset(new_biter);
    if (!s.ok()) {
      // The block is unreadable. Its rows cannot be skipped silently, so stop
      // the scan here and let the status propagate.
      status_ = s;
      valid_ = false;
      return;
    }
    for (biter_->SeekToFirst(); biter_->Valid(); biter_->Next()) {
      ParsedInternalKey ikey;
      Status ps = rocksdb::ParseInternalKey(biter_->key(), &ikey, false);
      if (!ps.ok()) continue;
      if (ikey.sequence > options_.read_seqno) continue;
      if (ikey.type == rocksdb::kTypeDeletion ||
          ikey.type == rocksdb::kTypeSingleDeletion)
        continue;
      if (SAICodec::Evaluate(
              query_,
              std::string_view(biter_->value().data(), biter_->value().size()),
              options_)) {
        PinnableSlice k;
        k.PinSelf(biter_->key());
        PinnableSlice v;
        v.PinSlice(biter_->value(), nullptr);
        keys_buf_.push_back(std::move(k));
        values_buf_.push_back(std::move(v));
      }
    }
    if (!keys_buf_.empty()) {
      buf_idx_ = 0;
      valid_ = true;
      return;
    }
  }
}

void SAITableIterator::LoadNextBlockIndexed() {
  // Consume the candidate rowId stream one data block at a time: all candidates
  // in the current block are fetched in a single forward walk (candidates are
  // ascending; rowId = key-order ordinal). NO full-query evaluation here —
  // post-filtering of every original predicate happens in SAIIterator's
  // MultiGet cross-check (SAI post-filter, design §5.3).
  while (cursor_ && cursor_->Valid()) {
    uint32_t block, ordinal;
    idx_->Locate(cursor_->Row(), block, ordinal);
    // Collect this block's candidate ordinals.
    std::vector<uint32_t> ordinals{ordinal};
    cursor_->Next();
    while (cursor_->Valid()) {
      uint32_t b2, o2;
      idx_->Locate(cursor_->Row(), b2, o2);
      if (b2 != block) break;
      ordinals.push_back(o2);
      cursor_->Next();
    }
    keys_buf_.clear();
    values_buf_.clear();
    Status s;
    BlockHandle bh;
    bh.set_offset(idx_->block_handles[block].offset);
    bh.set_size(idx_->block_handles[block].size);
    // Same standard readahead as the full-scan path above; in index mode the
    // candidate blocks are usually sparse, which PrefetchIfNeeded detects and
    // leaves unprefetched.
    ReadOptions read_options;
    block_prefetcher_.PrefetchIfNeeded(
        bbt_->get_rep(), bh, read_options.readahead_size,
        /*is_for_compaction=*/false, /*no_sequential_checking=*/false,
        read_options, /*readaheadsize_cb=*/nullptr,
        /*is_async_io_prefetch=*/false);
    DataBlockIter* new_biter = bbt_->NewDataBlockIterator<DataBlockIter>(
        read_options, bh, nullptr, BlockType::kData, nullptr, nullptr,
        block_prefetcher_.prefetch_buffer(), false, false, s, true);
    biter_.reset(new_biter);
    if (!s.ok()) {
      // The block the candidate rowIds point at is unreadable. Its rows
      // cannot be skipped silently, so stop the scan and let the status
      // propagate.
      status_ = s;
      valid_ = false;
      return;
    }
    size_t want = 0;
    uint32_t walk = 0;
    for (biter_->SeekToFirst(); biter_->Valid() && want < ordinals.size();
         biter_->Next(), ++walk) {
      if (walk != ordinals[want]) continue;
      ++want;
      ParsedInternalKey ikey;
      Status ps = rocksdb::ParseInternalKey(biter_->key(), &ikey, false);
      if (!ps.ok()) continue;
      if (ikey.sequence > options_.read_seqno) continue;
      if (ikey.type == rocksdb::kTypeDeletion ||
          ikey.type == rocksdb::kTypeSingleDeletion)
        continue;
      PinnableSlice k;
      k.PinSelf(biter_->key());
      PinnableSlice v;
      v.PinSlice(biter_->value(), nullptr);
      keys_buf_.push_back(std::move(k));
      values_buf_.push_back(std::move(v));
    }
    if (!keys_buf_.empty()) {
      buf_idx_ = 0;
      valid_ = true;
      return;
    }
    // Every candidate in this block was MVCC/tombstone-skipped; next block.
  }
  valid_ = false;
}

void SAITableIterator::GetReadaheadState(
    ReadaheadFileInfo* readahead_file_info) {
  if (block_prefetcher_.prefetch_buffer() != nullptr) {
    block_prefetcher_.prefetch_buffer()->GetReadaheadState(
        &(readahead_file_info->data_block_readahead_info));
  }
}

void SAITableIterator::SetReadaheadState(
    ReadaheadFileInfo* readahead_file_info) {
  // A zero readahead_size means no prior file built a prefetch buffer;
  // applying it would set this file's initial size to 0 and disable
  // auto-readahead entirely, so keep the fresh-start defaults instead.
  if (readahead_file_info->data_block_readahead_info.readahead_size > 0) {
    block_prefetcher_.SetReadaheadState(
        &(readahead_file_info->data_block_readahead_info));
  }
}

void SAITableIterator::SeekToFirst() {
  // A failure is sticky: never restart a scan that already lost data.
  if (!status_.ok()) {
    valid_ = false;
    return;
  }
  // The SAI block failed to load in the constructor: nothing to scan.
  if (idx_ == nullptr) {
    valid_ = false;
    return;
  }
  cur_block_idx_ = -1;
  buf_idx_ = 0;
  keys_buf_.clear();
  values_buf_.clear();
  valid_ = false;
  if (plan_.full_scan) {
    LoadNextBlockScan();
  } else {
    LoadNextBlockIndexed();
  }
}

void SAITableIterator::Next() {
  assert(valid_);
  ++buf_idx_;
  if (buf_idx_ >= static_cast<int32_t>(keys_buf_.size())) {
    valid_ = false;
    if (plan_.full_scan) {
      LoadNextBlockScan();
    } else {
      LoadNextBlockIndexed();
    }
  }
}

Slice SAITableIterator::key() const {
  assert(Valid());
  return keys_buf_[buf_idx_];
}

Slice SAITableIterator::value() const {
  assert(Valid());
  return values_buf_[buf_idx_];
}

}  // namespace experiment::sai
