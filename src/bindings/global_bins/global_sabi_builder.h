#pragma once
// Fork of bit_lsm::SABIBuilder for the global-bins ablation.
//
// FORKED FROM third_party/BitLSM @ 168d595 (src/include/sabi.h SABIBuilder,
// src/include/sabi_builder.cpp). Everything except SetBinningPolicy() is a
// verbatim copy, so the blob this builder writes is the core's v8 layout and
// the unmodified SABIReader opens it. global_bins_xcheck proves the copy is
// byte-identical to the core builder; re-run it after every submodule bump.
//
// With no policy the builder behaves exactly like the core (per-SST local
// bins). With a policy it takes the bin counts and boundaries from the oracle
// BinPolicy instead of from this SST's rows, and keeps the per-SST pruning the
// reader derives from the blob:
//   - kRange: the global boundaries are clamped to this SST's [min, max], so
//     the first and last boundary are still the SST's own bounds (the reader's
//     min/max skip), while every interior boundary is the global one.
//   - kEquality: only the values present in this SST are listed, so the
//     reader's "value not in this SST" skip still fires.
// A value outside the policy's range, or a categorical value the policy never
// saw, cannot happen with an oracle policy and aborts the build.
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "bin_policy.h"
#include "sabi.h"

namespace experiment::global_bins {

using bit_lsm::AttrExtractor;
using bit_lsm::BitmapIndex;
using bit_lsm::BytesList;
using bit_lsm::EncodedAttr;
using bit_lsm::SABISchema;

class GlobalSABIBuilder : public rocksdb::UserDefinedIndexBuilder {
 private:
  SABISchema schema_;
  std::unique_ptr<AttrExtractor> extractor_;  // exclusively owned
  std::vector<EncodedAttr>
      scratch_;  // per-row extraction buffer, attr_num slots
  // Oracle policy; nullptr = the core's per-SST binning.
  std::shared_ptr<const BinPolicy> policy_;

  // Interned buffer for one unordered attribute: each distinct value is
  // appended once to a string arena and rows keep only its dense id. The
  // value -> id lookup is a flat open-addressing table so high-cardinality
  // attributes pay no per-value node allocation.
  struct EqualityAttrBuf {
    struct ValueRef {
      uint32_t offset;
      uint32_t len;
    };
    std::string arena;  // concatenated distinct values
    std::vector<ValueRef> value_by_id;
    std::vector<uint32_t> count_by_id;
    std::vector<uint32_t> row_ids;    // row -> id
    std::vector<uint32_t> bin_by_id;  // id -> bin, set by binning policy

    std::string_view ValueOf(uint32_t id) const {
      const ValueRef& v = value_by_id[id];
      return std::string_view(arena.data() + v.offset, v.len);
    }
    void Intern(std::string_view value);

   private:
    static constexpr size_t kInitSlots = 1024;  // power of two
    std::vector<uint64_t> slot_hash_ = std::vector<uint64_t>(kInitSlots);
    std::vector<uint32_t> slot_id_ =
        std::vector<uint32_t>(kInitSlots, 0);  // id + 1; 0 = empty slot
    size_t used_ = 0;

    void Grow();
  };

  // Dense per-row buffer for one kRange attribute: values as bytes, append
  // only, so a row costs one memcpy. Sort() fills `sorted` (row positions in
  // value order) and `distinct`; the bin budget and the binning sweep both
  // read those, so the sort happens once.
  struct RangeAttrBuf {
    BytesList values;
    std::vector<uint32_t> sorted;     // row positions in value order
    std::vector<uint32_t> run_start;  // equal-value runs in `sorted`, plus n
    uint64_t distinct = 0;
    // Length shared by every value so far: -2 while empty, -1 once two
    // lengths differ. Uniform 8-byte values (every numeric attr) sort as
    // contiguous okeys instead of through arena offsets.
    int32_t uniform_len = -2;
    // Dense row -> local bin, filled by SetRangeBinningPolicy in one walk
    // over the sorted runs against the final boundaries.
    std::vector<uint32_t> bin_of_row;
    void Push(std::string_view v);
    void Sort();
  };

  // Per-attr value buffer, dense: data rows only (kRange bytes; kEquality
  // interned bytes). NULL and tombstone rows push nothing — they
  // live solely in attr_null_rows_ / tombstone_bitmap — so binning statistics
  // are plain scans. CalculateBitmapIndex is the single place that re-aligns
  // buffer entries with row ids by skipping exactly those bitmaps' ids.
  std::vector<std::variant<EqualityAttrBuf, RangeAttrBuf>> attr_buf_;
  // Per-attr set of row ids whose value is SQL NULL. NULL rows land in no
  // value bin and never enter binning-boundary estimation, so range/equality
  // queries auto-exclude them. Empty for non-nullable attrs.
  std::vector<roaring::Roaring> attr_null_rows_;

  // Statistics
  uint64_t total_data_entries_size_uncomp_ = 0;  // total size of KVPs (bytes)
  uint32_t data_entries_cnt_ = 0;   // total number of KVPs in current table
  uint32_t index_entries_cnt_ = 0;  // total number of index entries added
  // Per-attr exact distinct values (directory field). kRange: the sorted
  // buffer's run count; kEquality: the interning table size.
  std::vector<uint64_t> distinct_cnts_;

  // Bitmap Index
  BitmapIndex bitmap_index_;

  // Index blob
  std::string index_blob_;

  // Helper methods
  void SetBinningPolicy();
  void SetEqualityBinningPolicy(uint32_t i);
  void SetRangeBinningPolicy(uint32_t i);
  void CalculateBitmapIndex();
  // Global-bins replacements for the three binning steps above.
  void SetGlobalBinningPolicy();
  void SetGlobalEqualityBinningPolicy(uint32_t i);
  void SetGlobalRangeBinningPolicy(uint32_t i);

 public:
  GlobalSABIBuilder(SABISchema schema, std::unique_ptr<AttrExtractor> extractor,
                    std::shared_ptr<const BinPolicy> policy);
  rocksdb::Slice AddIndexEntry(
      const rocksdb::Slice& last_key_in_current_block,
      const rocksdb::Slice* first_key_in_next_block,
      const rocksdb::UserDefinedIndexBuilder::BlockHandle& block_handle,
      std::string* separator_scratch) override;
  void OnKeyAdded(const rocksdb::Slice& key, ValueType type,
                  const rocksdb::Slice& value) override;
  rocksdb::Status Finish(rocksdb::Slice* index_contents) override;

  // Policy tool: run the core's per-SST binning over every row added so far
  // and return it as a BinPolicy. Feeding the whole workload through here is
  // what makes the oracle policy the core's own algorithm. Only valid on a
  // builder without a policy; the builder cannot Finish() afterwards.
  BinPolicy ExportLocalPolicy();
};

}  // namespace experiment::global_bins
