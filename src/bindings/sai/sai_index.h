#pragma once
// SAI per-SSTable index attached via RocksDB UserDefinedIndex.
// Corresponds in ROLE to Cassandra's StorageAttachedIndexWriter + V1OnDiskFormat
// per-column components (src/java/org/apache/cassandra/index/sai/disk/):
// dense rowIds assigned in key order at flush/compaction
// (StorageAttachedIndexWriter.java:277-288), one segment per SSTable (design D6).
// Mirrors the embedded baseline's EmbeddedIndexBuilder/Reader/Factory structure
// (src/bindings/embedded/embedded_index.h).
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "bit_lsm_option.h"
#include "rocksdb/user_defined_index.h"
#include "sai_cont_index.h"
#include "sai_plan.h"
#include "sai_postings.h"
#include "sai_trie.h"

namespace experiment::sai {

class SAIIndexBuilder : public rocksdb::UserDefinedIndexBuilder {
 public:
  explicit SAIIndexBuilder(const bit_lsm::BitLSMOptions& options);
  rocksdb::Slice AddIndexEntry(const rocksdb::Slice& last_key_in_current_block,
                               const rocksdb::Slice* first_key_in_next_block,
                               const BlockHandle& block_handle,
                               std::string* separator_scratch) override;
  // ValueType is UserDefinedIndexBuilder's NESTED enum (user_defined_index.h:40,
  // members kValue/kTypeDeletion/...), resolved via inheritance — do NOT qualify
  // as rocksdb::ValueType (that is the unrelated dbformat enum).
  void OnKeyAdded(const rocksdb::Slice& key, ValueType type,
                  const rocksdb::Slice& value) override;
  rocksdb::Status Finish(rocksdb::Slice* index_contents) override;

 private:
  bit_lsm::BitLSMOptions options_;
  uint32_t entries_total_ = 0;  // == next rowId; counts every entry (psum alignment)
  struct BlockEntry { uint32_t psum; uint64_t off; uint64_t size; };
  std::vector<BlockEntry> blocks_;
  // Per-attr accumulators (index into these = attr_idx).
  std::vector<std::map<std::string, PostingsBuilder, std::less<>>> cat_terms_;
  std::vector<ContWriter> cont_;
  std::string blob_;  // owns the serialized result from Finish()
};

class SAIIndexReader : public rocksdb::UserDefinedIndexReader {
 public:
  SAIIndexReader(rocksdb::Slice& index_block,
                 const bit_lsm::BitLSMOptions& options);
  std::unique_ptr<rocksdb::UserDefinedIndexIterator> NewIterator(
      const rocksdb::ReadOptions&) override {
    return nullptr;  // scanning is driven by SAITableIterator, not this.
  }
  // The block cache charges this on top of the raw block bytes
  // (Block_kUserDefinedIndex::ApproximateMemoryUsage), so it must cover every
  // heap structure the reader keeps resident: the blob copy plus the Section-A
  // vectors parsed out of it in the constructor. Undercounting here leaks
  // memory past the block_cache budget.
  size_t ApproximateMemoryUsage() const override {
    return sizeof(*this) + owned_.capacity() +
           entry_count_psum.capacity() * sizeof(uint32_t) +
           block_handles.capacity() *
               sizeof(rocksdb::UserDefinedIndexBuilder::BlockHandle) +
           region_off_.capacity() * sizeof(uint32_t);
  }

  uint64_t Estimate(const SAIFact& f) const;
  std::unique_ptr<RowCursor> OpenCursor(const SAIFact& f) const;
  void Locate(uint32_t row, uint32_t& block_idx, uint32_t& ordinal) const;
  uint32_t EntriesTotal() const { return entries_total_; }

  std::vector<uint32_t> entry_count_psum;  // size B, cumulative through block i
  std::vector<rocksdb::UserDefinedIndexBuilder::BlockHandle> block_handles;

 private:
  const char* Region(uint32_t attr_idx) const { return base_ + region_off_[attr_idx]; }
  bit_lsm::BitLSMOptions options_;
  std::string owned_;
  const char* base_ = nullptr;
  std::vector<uint32_t> region_off_;
  uint32_t entries_total_ = 0;
};

class SAIIndexFactory : public rocksdb::UserDefinedIndexFactory {
 public:
  explicit SAIIndexFactory(const bit_lsm::BitLSMOptions& options)
      : options_(options) {}
  const char* Name() const override { return "SAIIndexFactory"; }
  rocksdb::UserDefinedIndexBuilder* NewBuilder() const override;
  std::unique_ptr<rocksdb::UserDefinedIndexReader> NewReader(
      rocksdb::Slice& index_block) const override;

 private:
  bit_lsm::BitLSMOptions options_;
};

}  // namespace experiment::sai
