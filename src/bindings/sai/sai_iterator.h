#pragma once
// Corresponds to the embedded baseline's iterator stack
// (src/bindings/embedded/embedded_iterator.h), itself a port of BitLSM's
// bit_lsm_iterator.h. Differences for SAI: an SAIPlan (chosen index facts) is
// threaded down to SAITableIterator, which replaces block-pruning-and-scan with
// posting-intersection candidate generation (Cassandra SAI architecture: index
// search -> candidate primary keys -> materialize -> post-filter).
#include <bit_lsm_query.h>
#include <cstdint>
#include <queue>
#include "bit_lsm_option.h"
#include "db/column_family.h"
#include "db/db_impl/db_impl.h"
#include "db/memtable.h"
#include "db/version_set.h"
#include "sai_index.h"
#include "sai_plan.h"
#include "table/block_based/block_based_table_reader.h"

namespace experiment::sai {

class SAIInternalIterator {
 protected:
  bool valid_ = false;

 public:
  virtual ~SAIInternalIterator() {}
  virtual void SeekToFirst() = 0;
  virtual void Next() = 0;
  bool Valid() const { return valid_; }
  virtual rocksdb::Slice key() const = 0;
  virtual rocksdb::Slice value() const = 0;
};

class SAIIterator;
class SAIMergingIterator;
class SAILevelIterator;
class SAITableIterator;
class SAIMemTableIterator;

// Memtable scan + exact predicate evaluation (design D1: no memtable term
// index; corresponds to the embedded baseline's EmbeddedMemTableIterator).
class SAIMemTableIterator : public SAIInternalIterator {
 private:
  bit_lsm::BitLSMOptions options_;
  rocksdb::MemTable* mem_;
  bit_lsm::BitLSMQuery query_;
  rocksdb::Arena arena_;
  rocksdb::InternalIterator* iter_ = nullptr;
  void FindNextValidEntry();

 public:
  SAIMemTableIterator(rocksdb::MemTable* mem, bit_lsm::BitLSMOptions options,
                      bit_lsm::BitLSMQuery query);
  ~SAIMemTableIterator() override;
  void SeekToFirst() override;
  void Next() override;
  rocksdb::Slice key() const override;
  rocksdb::Slice value() const override;
};

// Per-SST iterator. Index mode (plan.full_scan == false): open posting cursors
// for the chosen facts, intersect, fetch candidate rows by (block, ordinal) —
// NO full-query evaluation here; the post-filter lives in SAIIterator's
// MultiGet cross-check (corresponds to Cassandra's FilterTree re-verification,
// plan/StorageAttachedIndexSearcher.java:497-578). Full-scan mode
// (plan.full_scan == true): scan every block and evaluate exactly, identical
// to EmbeddedTableIterator's candidate-block scan.
class SAITableIterator : public SAIInternalIterator {
 private:
  bit_lsm::BitLSMOptions options_;
  rocksdb::BlockBasedTable* bbt_;
  rocksdb::BlockBasedTable::IndexReader* index_reader_;
  SAIIndexReader* idx_;
  bit_lsm::BitLSMQuery query_;
  SAIPlan plan_;

  std::unique_ptr<RowCursor> cursor_;  // index mode: candidate rowId stream
  int32_t cur_block_idx_ = -1;         // full-scan mode block cursor

  std::unique_ptr<rocksdb::DataBlockIter> biter_;
  std::vector<rocksdb::PinnableSlice> keys_buf_;
  std::vector<rocksdb::PinnableSlice> values_buf_;
  int32_t buf_idx_ = 0;

  void BuildCursor();          // index mode ctor helper
  void LoadNextBlockScan();    // full-scan mode (EmbeddedTableIterator port)
  void LoadNextBlockIndexed(); // index mode: next block holding candidates (Task 8)

 public:
  SAITableIterator(rocksdb::BlockBasedTable* bbt, bit_lsm::BitLSMOptions options,
                   bit_lsm::BitLSMQuery query, SAIPlan plan);
  void SeekToFirst() override;
  void Next() override;
  rocksdb::Slice key() const override;
  rocksdb::Slice value() const override;
};

struct SAIIteratorComparator {
  const rocksdb::InternalKeyComparator* icmp_;
  SAIIteratorComparator(const rocksdb::InternalKeyComparator* icmp = nullptr)
      : icmp_(icmp) {}
  bool operator()(const SAIInternalIterator* a,
                  const SAIInternalIterator* b) const {
    return icmp_->Compare(a->key(), b->key()) > 0;
  }
};

class SAILevelIterator : public SAIInternalIterator {
 private:
  uint32_t level_;
  bit_lsm::BitLSMOptions options_;
  bit_lsm::BitLSMQuery query_;
  SAIPlan plan_;
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
  SAITableIterator* cur_sti_;
  void LoadFile(size_t idx);

 public:
  SAILevelIterator(rocksdb::SuperVersion* sv, uint32_t level,
                   bit_lsm::BitLSMOptions options, bit_lsm::BitLSMQuery query,
                   SAIPlan plan);
  ~SAILevelIterator() override;
  void SeekToFirst() override;
  void Next() override;
  rocksdb::Slice key() const override;
  rocksdb::Slice value() const override;
};

class SAIMergingIterator : public SAIInternalIterator {
 private:
  bit_lsm::BitLSMOptions options_;
  bit_lsm::BitLSMQuery query_;
  SAIPlan plan_;
  rocksdb::SuperVersion* sv_;
  rocksdb::ColumnFamilyData* cfd_;
  rocksdb::Version* v_;
  rocksdb::TableCache* tc_;
  const rocksdb::VersionStorageInfo* storage_info_;
  const rocksdb::InternalKeyComparator* icmp_;
  const rocksdb::MutableCFOptions& cf_opts_;
  std::vector<rocksdb::TableCache::TypedHandle*> l0_handles_;
  std::vector<SAIInternalIterator*> ch_iters_;
  std::priority_queue<SAIInternalIterator*, std::vector<SAIInternalIterator*>,
                      SAIIteratorComparator>
      heap_;

 public:
  SAIMergingIterator(rocksdb::SuperVersion* sv, bit_lsm::BitLSMOptions options,
                     bit_lsm::BitLSMQuery query, SAIPlan plan);
  ~SAIMergingIterator() override;
  void SeekToFirst() override;
  void Next() override;
  rocksdb::Slice key() const override;
  rocksdb::Slice value() const override;
};

// Top-level iterator: global key-order merge, dedup (newest version first),
// candidate materialization via MultiGet + full-query re-verification.
// The MultiGet + SAICodec::Evaluate step corresponds to Cassandra SAI's row
// materialization + FilterTree post-filter over ALL predicates.
class SAIIterator : public SAIInternalIterator {
 private:
  rocksdb::DB* db_;
  rocksdb::DBImpl* db_impl_;
  rocksdb::ColumnFamilyHandle* cfh_;
  const rocksdb::Snapshot* snapshot_;
  rocksdb::ColumnFamilyData* cfd_;
  rocksdb::SuperVersion* sv_;
  SAIMergingIterator* smi_;
  bit_lsm::BitLSMOptions options_;
  bit_lsm::BitLSMQuery query_;
  int intersection_limit_;
  SAIPlan plan_;
  std::vector<std::string> batch_keys_;
  std::vector<std::string> batch_values_;
  uint32_t batch_cur_idx_ = 0;
  std::string latest_user_key_added;
  void BuildPlan();  // ranking pass (Task 8; full_scan placeholder in Task 7)
  void FetchNextBatch(uint32_t batch_size);

 public:
  SAIIterator(rocksdb::DB* db, rocksdb::ColumnFamilyHandle* cfh,
              bit_lsm::BitLSMOptions options, bit_lsm::BitLSMQuery query,
              int intersection_limit);
  ~SAIIterator() override;
  void SeekToFirst() override;
  void Next() override;
  rocksdb::Slice key() const override;
  rocksdb::Slice value() const override;
};

}  // namespace experiment::sai
