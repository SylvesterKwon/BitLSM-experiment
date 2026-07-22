// Per-SST iterator for the SAI baseline.
// Full-scan mode (this task): port of EmbeddedTableIterator's block scan
// (src/bindings/embedded/embedded_table_iterator.cpp), candidate set = ALL
// blocks, exact SAICodec::Evaluate per entry.
// Index mode (Task 8): posting intersection -> candidate rowIds -> (block,
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
      index_reader_(bbt_->get_rep()->index_reader.get()),
      idx_(static_cast<SAIIndexReader*>(index_reader_->GetUDIReader())) {
  if (!plan_.full_scan) BuildCursor();
}

void SAITableIterator::BuildCursor() {
  cursor_.reset();  // Task 8 implements index-mode cursor construction
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
    DataBlockIter* new_biter = bbt_->NewDataBlockIterator<DataBlockIter>(
        ReadOptions(), bh, nullptr, BlockType::kData, nullptr, nullptr, nullptr,
        false, false, s, true);
    biter_.reset(new_biter);
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
  valid_ = false;  // Task 8 implements
}

void SAITableIterator::SeekToFirst() {
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
