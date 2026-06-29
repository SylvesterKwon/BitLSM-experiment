#pragma once
// Embedded Index (Qader SIGMOD'18): per-block Bloom Filter (categorical) +
// per-block raw Zone Map (continuous), plus continuous-only file-level zone map.
// Embedded in each SSTable via RocksDB's UserDefinedIndex.
// Corresponds in ROLE to BitLSM's SABIBuilder/SABIReader/SABIFactory
// (third_party/BitLSM/src/include/sabi.h) but stores BF+zonemap, not bitmaps,
// and is fully decoupled from BitLSM internals.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "rocksdb/user_defined_index.h"
#include "bit_lsm_option.h"

namespace experiment::embedded {

struct ZoneMap {
  double min;
  double max;
  bool overlaps(double lo, bool lo_inc, double hi, bool hi_inc) const {
    bool below = lo_inc ? (max < lo) : (max <= lo);
    bool above = hi_inc ? (min > hi) : (min >= hi);
    return !(below || above);
  }
};

class EmbeddedIndexReader : public rocksdb::UserDefinedIndexReader {
 public:
  EmbeddedIndexReader(rocksdb::Slice& index_block,
                      const bit_lsm::BitLSMOptions& options, uint32_t bloom_bits);
  std::unique_ptr<rocksdb::UserDefinedIndexIterator> NewIterator(
      const rocksdb::ReadOptions&) override {
    return nullptr;  // pruning is driven by EmbeddedTableIterator, not this.
  }
  size_t ApproximateMemoryUsage() const override { return raw_len_; }

  // Region of Section C for block_idx; len set to its byte length.
  const char* BlockFilterRegion(uint32_t block_idx, size_t& len) const {
    len = section_c_off_[block_idx + 1] - section_c_off_[block_idx];
    return base_ + section_c_off_[block_idx];
  }
  bool FileZoneOverlaps(uint32_t cont_ordinal, double lo, bool lo_inc,
                        double hi, bool hi_inc) const {
    return file_zone_[cont_ordinal].overlaps(lo, lo_inc, hi, hi_inc);
  }

  std::vector<uint32_t> entry_count_psum;                           // size B; retained for parity/debuggability (table iterator scans full candidate blocks)
  std::vector<rocksdb::UserDefinedIndexBuilder::BlockHandle> block_handles; // size B
  bit_lsm::BitLSMOptions options;
  uint32_t bloom_bits;

 private:
  const char* base_ = nullptr;
  size_t raw_len_ = 0;
  std::vector<uint32_t> section_c_off_;  // size B+1, absolute offsets into blob
  std::vector<ZoneMap> file_zone_;       // size = #continuous attrs
  std::string owned_;                    // owns the blob bytes
};

class EmbeddedIndexBuilder : public rocksdb::UserDefinedIndexBuilder {
 public:
  EmbeddedIndexBuilder(const bit_lsm::BitLSMOptions& options, uint32_t bloom_bits);
  rocksdb::Slice AddIndexEntry(const rocksdb::Slice& last_key_in_current_block,
                               const rocksdb::Slice* first_key_in_next_block,
                               const BlockHandle& block_handle,
                               std::string* separator_scratch) override;
  void OnKeyAdded(const rocksdb::Slice& key, ValueType type,
                  const rocksdb::Slice& value) override;
  rocksdb::Status Finish(rocksdb::Slice* index_contents) override;

 private:
  void FlushCurrentBlock(const BlockHandle& bh);

  bit_lsm::BitLSMOptions options_;
  uint32_t bloom_bits_;
  uint32_t entries_total_ = 0;

  // Per-current-block accumulators (reset on AddIndexEntry).
  std::vector<class BloomBuilder> cat_bloom_;   // one per attr; categorical used
  std::vector<ZoneMap> cont_zone_;              // one per attr; continuous used
  std::vector<bool> cont_seen_;                 // first value flag per attr

  // File-level continuous zone maps + first-seen flags.
  std::vector<ZoneMap> file_zone_;
  std::vector<bool> file_seen_;

  // Section A entries.
  struct BlockEntry { uint32_t psum; uint64_t off; uint64_t size; };
  std::vector<BlockEntry> blocks_;
  // Section C payloads, one string per block (already serialized).
  std::vector<std::string> block_payloads_;

  // Owns the serialized blob returned by Finish().
  std::string blob_;
};

class EmbeddedIndexFactory : public rocksdb::UserDefinedIndexFactory {
 public:
  EmbeddedIndexFactory(const bit_lsm::BitLSMOptions& options, uint32_t bloom_bits)
      : options_(options), bloom_bits_(bloom_bits) {}
  const char* Name() const override { return "EmbeddedIndexFactory"; }
  rocksdb::UserDefinedIndexBuilder* NewBuilder() const override;
  std::unique_ptr<rocksdb::UserDefinedIndexReader> NewReader(
      rocksdb::Slice& index_block) const override;

 private:
  bit_lsm::BitLSMOptions options_;
  uint32_t bloom_bits_;
};

}  // namespace experiment::embedded
