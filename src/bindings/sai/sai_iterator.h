#pragma once
// Corresponds to the embedded baseline's iterator stack
// (src/bindings/embedded/embedded_iterator.h), itself a port of BitLSM's
// bit_lsm_iterator.h. Differences for SAI: an SAIPlan (chosen index facts) is
// threaded down to SAITableIterator, which replaces block-pruning-and-scan with
// posting-intersection candidate generation (Cassandra SAI architecture: index
// search -> candidate primary keys -> materialize -> post-filter).
#include <bit_lsm_query.h>
#include <bit_lsm_shadow_check.h>
#include <cstdint>
#include <queue>
#include "bit_lsm_option.h"
#include "bit_lsm_utils.h"
#include "db/column_family.h"
#include "db/db_impl/db_impl.h"
#include "db/memtable.h"
#include "db/version_set.h"
#include "file/readahead_file_info.h"
#include "block_prefetch_queue.h"
#include "sai_index.h"
#include "sai_plan.h"
#include "table/block_based/block_based_table_reader.h"
#include "table/block_based/block_prefetcher.h"

namespace experiment::sai {

// Source level reported by iterators whose row came from a memtable rather
// than an SST. Mirrors BitLSM's kMemtableSourceLevel.
inline constexpr int kMemtableSourceLevel = -1;

class SAIInternalIterator {
 protected:
  bool valid_ = false;
  // OK unless iteration hit an error. Sticky: once an iterator fails it stays
  // failed, because the failure (a SAI block that will not load) is not
  // recoverable by re-seeking the same iterator. Every layer must therefore
  // distinguish "!Valid() and OK" (exhausted) from "!Valid() and !OK"
  // (stopped early), never treating the latter as end-of-data.
  rocksdb::Status status_;

 public:
  virtual ~SAIInternalIterator() {}
  virtual void SeekToFirst() = 0;
  virtual void Next() = 0;
  bool Valid() const { return valid_; }
  virtual rocksdb::Status status() const { return status_; }
  virtual rocksdb::Slice key() const = 0;
  virtual rocksdb::Slice value() const = 0;
  // Provenance of the current row, used by SAIIterator's shadow check to
  // decide whether the authoritative MultiGet can be skipped. Defaults suit a
  // memtable source, which has no file and whose rows the checker probes
  // directly. Mirrors BitLSM's SABIInternalIterator (PR #41).
  virtual int SourceLevel() const { return kMemtableSourceLevel; }
  virtual uint64_t SourceFileNumber() const { return 0; }
  // True when a newer version of this row exists in the SAME file, which the
  // checker cannot see: it deliberately ignores the candidate's own file.
  virtual bool SourceHasNewerVersion() const { return false; }
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
  // Derived from the schema once: the row codec takes a cached
  // layout, and building one per row costs four vector allocations.
  bit_lsm::ValueLayout layout_{options_};
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
  // Derived from the schema once: the row codec takes a cached
  // layout, and building one per row costs four vector allocations.
  bit_lsm::ValueLayout layout_{options_};
  rocksdb::BlockBasedTable* bbt_;
  // Holds the SAI entry (and its parsed SAIIndexReader) for this iterator's
  // lifetime: a block cache pin when cache_index_and_filter_blocks is on
  // (evictable after release), or an unowned reference to the table-lifetime
  // pin in Rep when off. Either way valid only while the table reader stays
  // alive. Declared before every member that references reader state (the
  // posting/cont cursors point into the reader's blob) — members are
  // destroyed in reverse order, so this must die last.
  rocksdb::CachableEntry<rocksdb::Block_kUserDefinedIndex> udi_entry_;
  SAIIndexReader* idx_ = nullptr;  // points into udi_entry_; null on failure
  bit_lsm::BitLSMQuery query_;
  SAIPlan plan_;

  std::unique_ptr<RowCursor> cursor_;  // index mode: candidate rowId stream
  int32_t cur_block_idx_ = -1;         // full-scan mode block cursor
  // Index mode: the rowId stream drained into (block, ordinal) pairs at
  // construction, so the candidate block set is known before the first read.
  // Parallel arrays in candidate order; cand_pos_ is the scan's cursor.
  std::vector<uint32_t> cand_block_;
  std::vector<uint32_t> cand_ordinal_;
  size_t cand_pos_ = 0;

  std::unique_ptr<rocksdb::DataBlockIter> biter_;
  // RocksDB's standard adaptive readahead for this scan's data block reads,
  // the same mechanism BlockBasedTableIterator uses and the one BitLSM wired
  // into SABITableIterator (BitLSM PR #38). Its FilePrefetchBuffer is created
  // lazily by PrefetchIfNeeded once the block access pattern turns
  // near-sequential, so a sparse candidate set never triggers readahead.
  rocksdb::BlockPrefetcher block_prefetcher_;
  // Same queue BitLSM's scan uses (block_prefetch_queue.h). MaterializeCandidates
  // already drains the posting intersection before the first block is read, so
  // the block set is known here too and the two methods differ in what they
  // select, not in how they fetch it.
  std::vector<std::pair<uint32_t, rocksdb::BlockHandle>> prefetch_targets_;
  bit_lsm::BlockPrefetchQueue prefetch_queue_;
  size_t prefetch_pos_ = 0;
  std::vector<rocksdb::PinnableSlice> keys_buf_;
  std::vector<rocksdb::PinnableSlice> values_buf_;
  int32_t buf_idx_ = 0;

  // Whether the block walk must record in-file shadowing; only the shadow
  // check needs it, so a scan without it pays nothing.
  bool track_source_versions_ = false;
  int source_level_ = kMemtableSourceLevel;
  uint64_t file_number_ = 0;
  // A range tombstone in this file can cover a row without leaving a point
  // entry, and neither the block walk nor ShadowChecker (which skips the
  // candidate's own file) can see it. Such a file is never provably clean.
  bool source_has_range_del_ = false;
  // Parallel to keys_buf_: 1 when a newer version of that row was seen
  // earlier in the same block (or could not be ruled out).
  std::vector<uint8_t> shadowed_buf_;
  // Previous entry's user key, copied because the block iterator decodes keys
  // in place and Next() overwrites the buffer.
  std::string prev_user_key_;

  void BuildCursor();          // index mode ctor helper
  void MaterializeCandidates();  // index mode: drain cursor to (block, ord)
  void LoadNextBlockScan();    // full-scan mode (EmbeddedTableIterator port)
  void LoadNextBlockIndexed(); // index mode: next block holding candidates

 public:
  SAITableIterator(rocksdb::BlockBasedTable* bbt, bit_lsm::BitLSMOptions options,
                   bit_lsm::BitLSMQuery query, SAIPlan plan,
                   bool track_source_versions = false,
                   int source_level = kMemtableSourceLevel,
                   uint64_t file_number = 0,
                   bool source_has_range_del = false);
  void SeekToFirst() override;
  void Next() override;
  rocksdb::Slice key() const override;
  rocksdb::Slice value() const override;
  int SourceLevel() const override { return source_level_; }
  uint64_t SourceFileNumber() const override { return file_number_; }
  bool SourceHasNewerVersion() const override {
    if (!track_source_versions_) return false;
    return source_has_range_del_ || shadowed_buf_[buf_idx_] != 0;
  }
  // Carry the adaptive-readahead ramp across the files of a level scan, the
  // way LevelIterator hands ReadaheadFileInfo between BlockBasedTableIterators
  // so a new file resumes at the ramped readahead size instead of 8K
  // (BitLSM PR #39).
  void GetReadaheadState(rocksdb::ReadaheadFileInfo* readahead_file_info);
  void SetReadaheadState(rocksdb::ReadaheadFileInfo* readahead_file_info);
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
  // Readahead state of the last file whose scan built a prefetch buffer,
  // handed to each newly opened file so the ramp survives file switches.
  rocksdb::ReadaheadFileInfo readahead_file_info_;
  bool track_source_versions_;
  void LoadFile(size_t idx);

 public:
  SAILevelIterator(rocksdb::SuperVersion* sv, uint32_t level,
                   bit_lsm::BitLSMOptions options, bit_lsm::BitLSMQuery query,
                   SAIPlan plan, bool track_source_versions = false);
  ~SAILevelIterator() override;
  void SeekToFirst() override;
  void Next() override;
  rocksdb::Slice key() const override;
  rocksdb::Slice value() const override;
  int SourceLevel() const override;
  uint64_t SourceFileNumber() const override;
  bool SourceHasNewerVersion() const override;
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
                     bit_lsm::BitLSMQuery query, SAIPlan plan,
                     bool track_source_versions = false);
  ~SAIMergingIterator() override;
  void SeekToFirst() override;
  void Next() override;
  rocksdb::Slice key() const override;
  rocksdb::Slice value() const override;
  int SourceLevel() const override { return heap_.top()->SourceLevel(); }
  uint64_t SourceFileNumber() const override {
    return heap_.top()->SourceFileNumber();
  }
  bool SourceHasNewerVersion() const override {
    return heap_.top()->SourceHasNewerVersion();
  }
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
  // Derived from the schema once: the row codec takes a cached
  // layout, and building one per row costs four vector allocations.
  bit_lsm::ValueLayout layout_{options_};
  bit_lsm::BitLSMQuery query_;
  int intersection_limit_;
  SAIPlan plan_;
  std::vector<std::string> batch_keys_;
  std::vector<std::string> batch_values_;
  uint32_t batch_cur_idx_ = 0;
  std::string latest_user_key_added;

  // Shadow check (port of BitLSM PR #41): a candidate the scan already read is
  // the answer unless a newer version may live somewhere the per-SST scan
  // cannot see, so only the unprovable ones need the MultiGet re-fetch. Unlike
  // BitLSM and the embedded baseline, the post-filter still runs on every
  // clean row here: index mode deliberately leaves full-query evaluation to
  // this cross-check (design 5.3), so skipping the fetch must not skip the
  // predicate -- it just evaluates the row the scan already read.
  std::unique_ptr<bit_lsm::ShadowChecker> checker_;
  // Owns the ScanContext the checker holds by reference.
  std::unique_ptr<bit_lsm::ScanContext> scan_ctx_;
  bool check_enabled_ = false;
  // Whole-scan authority: on a settled DB no candidate can be shadowed at all,
  // so entire batches skip the re-fetch without any per-key probing.
  bool authoritative_scan_ = false;
  // Reused per batch so the candidate loop allocates nothing steady-state.
  std::vector<std::string> candidate_keys_;
  std::vector<std::string> candidate_values_;
  std::vector<rocksdb::SequenceNumber> candidate_seqnos_;
  std::vector<int> candidate_src_levels_;
  std::vector<uint64_t> candidate_src_files_;
  std::vector<uint8_t> candidate_in_file_shadowed_;
  std::vector<uint32_t> dirty_idx_;
  uint64_t checked_keys_ = 0;
  uint64_t skipped_keys_ = 0;

  void BuildPlan();  // ranking pass; full_scan is the no-usable-facts placeholder
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
  // Shadow-check instrumentation, mirroring BitLSMIterator's TEST_ accessors:
  // lets a test assert the skip actually ran rather than passing vacuously,
  // and lets an experiment report the baseline's skip rate.
  uint64_t TEST_SkippedKeys() const { return skipped_keys_; }
  uint64_t TEST_CheckedKeys() const { return checked_keys_; }
  bool TEST_CheckEnabled() const { return check_enabled_; }
  bool TEST_AuthoritativeScan() const { return authoritative_scan_; }
};

}  // namespace experiment::sai
