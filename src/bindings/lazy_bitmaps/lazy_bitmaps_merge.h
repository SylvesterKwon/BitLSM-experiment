#pragma once
// OR merge operator of the lazy_bitmaps column family. Every put appends the
// bitmap {rowid} as one operand; flush, compaction and Get fold operands with
// bitwise OR. OR is associative and commutative, so partial merge is declared
// and an L0 file holds one merged operand per key.
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include <rocksdb/merge_operator.h>
#include <rocksdb/slice.h>

#include "lazy_bitmaps_keys.h"

namespace experiment::lazy_bitmaps {

// Operands folded by FullMergeV2 on the calling thread. RocksDB runs a Get's
// or iterator's FullMerge on the reading thread and a compaction's on a
// background thread, so resetting this before a query and reading it after
// counts that query's folds alone -- the spec's per-query operand count,
// without a Statistics object.
inline thread_local uint64_t tl_operands_folded = 0;

class RoaringOrMergeOperator : public rocksdb::MergeOperator {
 public:
  const char* Name() const override { return "RoaringOrMergeOperator"; }

  bool FullMergeV2(const MergeOperationInput& in,
                   MergeOperationOutput* out) const override {
    std::vector<roaring::Roaring> parts;
    parts.reserve(in.operand_list.size() + 1);
    if (in.existing_value)
      parts.push_back(ReadBitmap(in.existing_value->ToStringView()));
    for (const auto& op : in.operand_list)
      parts.push_back(ReadBitmap(op.ToStringView()));
    roaring::Roaring merged = Union(parts);
    WriteBitmap(merged, &out->new_value);
    tl_operands_folded += parts.size();
    return true;
  }

  bool PartialMergeMulti(const rocksdb::Slice& /*key*/,
                         const std::deque<rocksdb::Slice>& operands,
                         std::string* out,
                         rocksdb::Logger* /*logger*/) const override {
    std::vector<roaring::Roaring> parts;
    parts.reserve(operands.size());
    for (const auto& op : operands)
      parts.push_back(ReadBitmap(op.ToStringView()));
    roaring::Roaring merged = Union(parts);
    WriteBitmap(merged, out);
    return true;
  }

 private:
  static roaring::Roaring Union(const std::vector<roaring::Roaring>& parts) {
    if (parts.empty()) return roaring::Roaring();
    std::vector<const roaring::Roaring*> ptrs(parts.size());
    for (size_t i = 0; i < parts.size(); ++i) ptrs[i] = &parts[i];
    return roaring::Roaring::fastunion(ptrs.size(), ptrs.data());
  }
};

}  // namespace experiment::lazy_bitmaps
