#pragma once
// Corresponds to BitLSM's bit_lsm_iterator.h
// (third_party/BitLSM/src/include/bit_lsm_iterator.h); ported into namespace
// experiment::embedded, class names changed, and predicate swapped to
// EmbeddedCodec::Evaluate.

#include <bit_lsm_query.h>

#include <cstdint>
#include <queue>

#include "bit_lsm_option.h"
#include "db/column_family.h"
#include "db/db_impl/db_impl.h"
#include "db/memtable.h"
#include "db/version_set.h"
#include "embedded_index.h"
#include "table/block_based/block_based_table_reader.h"

namespace experiment::embedded {

// Abstract base class for internal iterators used by the embedded scan engine.
// Corresponds to BitLSM's SABIInternalIterator
// (third_party/BitLSM/src/include/bit_lsm_iterator.h); ported, predicate
// swapped to EmbeddedCodec::Evaluate.
class EmbeddedInternalIterator {
 protected:
  bool valid_ = false;

 public:
  virtual ~EmbeddedInternalIterator() {}
  virtual void SeekToFirst() = 0;
  virtual void Next() = 0;
  bool Valid() const { return valid_; }
  virtual rocksdb::Slice key() const = 0;
  virtual rocksdb::Slice value() const = 0;
};

// Forward declarations
class EmbeddedIterator;
class EmbeddedMergingIterator;
class EmbeddedLevelIterator;
class EmbeddedTableIterator;
class EmbeddedMemTableIterator;

// Memtable scan iterator for the embedded baseline.
// Corresponds to BitLSM's BitLSMMemTableIterator
// (third_party/BitLSM/src/include/bit_lsm_iterator.h); ported, predicate
// swapped to EmbeddedCodec::Evaluate.
class EmbeddedMemTableIterator : public EmbeddedInternalIterator {
 private:
  bit_lsm::BitLSMOptions options_;
  rocksdb::MemTable* mem_;
  bit_lsm::BitLSMQuery query_;

  // Internal status for iterating
  rocksdb::Arena arena_;
  rocksdb::InternalIterator* iter_ = nullptr;

  void FindNextValidEntry();

 public:
  EmbeddedMemTableIterator(rocksdb::MemTable* mem, bit_lsm::BitLSMOptions options,
                           bit_lsm::BitLSMQuery query);
  ~EmbeddedMemTableIterator() override;

  void SeekToFirst() override;
  void Next() override;
  rocksdb::Slice key() const override;
  rocksdb::Slice value() const override;
};

// Per-SST table iterator with BLOCK-LEVEL PRUNING for the embedded baseline.
// Corresponds to BitLSM's SABITableIterator
// (third_party/BitLSM/src/include/bit_lsm_iterator.h); block-pruning predicate
// is BF+raw zone map instead of a bitmap. SABI selects exact ROW indexes from a
// bitmap; this iterator selects candidate BLOCKS from BF/zonemap and then scans
// EVERY entry in each candidate block, applying EmbeddedCodec::Evaluate exactly.
class EmbeddedTableIterator : public EmbeddedInternalIterator {
 private:
  // Table & query context
  bit_lsm::BitLSMOptions options_;
  rocksdb::BlockBasedTable* bbt_;
  rocksdb::BlockBasedTable::IndexReader* index_reader_;
  EmbeddedIndexReader* idx_;
  bit_lsm::BitLSMQuery query_;

  // Candidate blocks selected by the flat-AND BF + zone-map predicate.
  std::vector<rocksdb::BlockHandle> candidate_blocks_;
  int32_t cur_block_idx_ = -1;  // current index into candidate_blocks_

  // Promoted to a member variable to enable zero-copy evaluation: keeping this
  // iterator alive pins the underlying data block, so PinSlice() values stay
  // valid (mirrors SABITableIterator).
  std::unique_ptr<rocksdb::DataBlockIter> biter_;
  std::vector<rocksdb::PinnableSlice> keys_buf_;
  std::vector<rocksdb::PinnableSlice> values_buf_;
  int32_t buf_idx_ = 0;  // cursor within the buffered matches

  // Build candidate_blocks_ from the per-block BF / zone-map regions.
  void SelectCandidateBlocks();
  // Open the next candidate block, scan all entries, buffer exact matches.
  void LoadNextBlock();

 public:
  EmbeddedTableIterator(rocksdb::BlockBasedTable* bbt,
                        bit_lsm::BitLSMOptions options,
                        bit_lsm::BitLSMQuery query);
  void SeekToFirst() override;
  void Next() override;
  rocksdb::Slice key() const override;
  rocksdb::Slice value() const override;
};

// IteratorComparator for EmbeddedMergingIterator's min-heap.
// Corresponds to BitLSM's IteratorComparator (bit_lsm_iterator.h).
struct EmbeddedIteratorComparator {
  const rocksdb::InternalKeyComparator* icmp_;
  EmbeddedIteratorComparator(
      const rocksdb::InternalKeyComparator* icmp = nullptr)
      : icmp_(icmp) {}
  bool operator()(const EmbeddedInternalIterator* a,
                  const EmbeddedInternalIterator* b) const {
    return icmp_->Compare(a->key(), b->key()) > 0;
  }
};

// Per-level SST iterator for the embedded baseline.
// Corresponds to BitLSM's BitLSMLevelIterator
// (third_party/BitLSM/src/include/bit_lsm_level_iterator.cpp); ported,
// SABITableIterator replaced with EmbeddedTableIterator.
class EmbeddedLevelIterator : public EmbeddedInternalIterator {
 private:
  uint32_t level_;
  bit_lsm::BitLSMOptions options_;
  bit_lsm::BitLSMQuery query_;
  rocksdb::SuperVersion* sv_;
  rocksdb::ColumnFamilyData* cfd_;
  rocksdb::Version* v_;
  rocksdb::TableCache* tc_;
  const rocksdb::VersionStorageInfo* storage_info_;
  const rocksdb::InternalKeyComparator* icmp_;
  const rocksdb::MutableCFOptions& cf_opts_;
  const std::vector<rocksdb::FileMetaData*>& files_;

  uint32_t cur_file_idx_;
  rocksdb::TableCache::TypedHandle* cur_table_handle_;
  EmbeddedTableIterator* cur_sti_;

  void LoadFile(size_t idx);

 public:
  EmbeddedLevelIterator(rocksdb::SuperVersion* sv, uint32_t level,
                        bit_lsm::BitLSMOptions options,
                        bit_lsm::BitLSMQuery query);
  ~EmbeddedLevelIterator() override;
  void SeekToFirst() override;
  void Next() override;
  rocksdb::Slice key() const override;
  rocksdb::Slice value() const override;
};

// Merging iterator for the embedded baseline.
// Corresponds to BitLSM's BitLSMMergingIterator
// (third_party/BitLSM/src/include/bit_lsm_merging_iterator.cpp); ported,
// SABITableIterator replaced with EmbeddedTableIterator,
// BitLSMMemTableIterator replaced with EmbeddedMemTableIterator.
class EmbeddedMergingIterator : public EmbeddedInternalIterator {
 private:
  bit_lsm::BitLSMOptions options_;
  bit_lsm::BitLSMQuery query_;
  rocksdb::SuperVersion* sv_;
  rocksdb::ColumnFamilyData* cfd_;
  rocksdb::Version* v_;
  rocksdb::TableCache* tc_;
  const rocksdb::VersionStorageInfo* storage_info_;
  const rocksdb::InternalKeyComparator* icmp_;
  const rocksdb::MutableCFOptions& cf_opts_;
  std::vector<rocksdb::TableCache::TypedHandle*> l0_handles_;

  std::vector<EmbeddedInternalIterator*> ch_iters_;
  std::priority_queue<EmbeddedInternalIterator*,
                      std::vector<EmbeddedInternalIterator*>,
                      EmbeddedIteratorComparator>
      heap_;

 public:
  EmbeddedMergingIterator(rocksdb::SuperVersion* sv,
                          bit_lsm::BitLSMOptions options,
                          bit_lsm::BitLSMQuery query);
  ~EmbeddedMergingIterator() override;
  void SeekToFirst() override;
  void Next() override;
  rocksdb::Slice key() const override;
  rocksdb::Slice value() const override;
};

// Top-level iterator for the embedded baseline.
// Corresponds to BitLSM's BitLSMIterator
// (third_party/BitLSM/src/include/bit_lsm_iterator.cpp); ported,
// BitLSMMergingIterator replaced with EmbeddedMergingIterator,
// value cross-check uses EmbeddedCodec::Evaluate via query_.CheckCondition.
class EmbeddedIterator : public EmbeddedInternalIterator {
 private:
  rocksdb::DB* db_;
  rocksdb::DBImpl* db_impl_;
  rocksdb::ColumnFamilyHandle* cfh_;
  const rocksdb::Snapshot* snapshot_;
  rocksdb::ColumnFamilyData* cfd_;
  rocksdb::SuperVersion* sv_;

  EmbeddedMergingIterator* smi_;
  bit_lsm::BitLSMOptions options_;
  bit_lsm::BitLSMQuery query_;

  std::vector<std::string> batch_keys_;
  std::vector<std::string> batch_values_;
  uint32_t batch_cur_idx_ = 0;

  std::string latest_user_key_added;

  void FetchNextBatch(uint32_t batch_size);

 public:
  EmbeddedIterator(rocksdb::DB* db, rocksdb::ColumnFamilyHandle* cfh,
                   bit_lsm::BitLSMOptions options, bit_lsm::BitLSMQuery query);
  ~EmbeddedIterator() override;
  void SeekToFirst() override;
  void Next() override;
  rocksdb::Slice key() const override;
  rocksdb::Slice value() const override;
};

}  // namespace experiment::embedded
