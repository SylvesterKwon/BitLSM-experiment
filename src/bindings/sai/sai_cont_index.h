#pragma once
// Continuous-attr index: flattened 1-D block balanced tree.
// Corresponds to Cassandra's BlockBalancedTreeWriter/Reader (1-D Lucene BKD
// specialisation, disk/v1/bbtree/BlockBalancedTreeWriter.java): 1024-entry
// value-sorted leaves, per-leaf rowId-sorted postings, and a LeafOrderMap
// permutation linking value order to posting order. The balanced split-value
// index is flattened into binary search over per-block min/max (design D2);
// internal-node pre-merged postings omitted (D2).
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#include "sai_postings.h"

namespace experiment::sai {

inline constexpr uint32_t kContBlockEntries = 1024;  // BlockBalancedTreeWriter.java:97

class ContWriter {
 public:
  void Add(double value, uint32_t row) { pts_.push_back({value, row}); }
  void AppendTo(std::string& out) const;  // full region (summaries+values+perm+postings)

 private:
  struct Pt { double v; uint32_t r; };
  mutable std::vector<Pt> pts_;  // sorted in AppendTo
};

class ContReader {
 public:
  explicit ContReader(const char* region);
  uint64_t Estimate(double lo, bool lo_inc, double hi, bool hi_inc) const;
  std::unique_ptr<RowCursor> OpenCursor(double lo, bool lo_inc, double hi,
                                        bool hi_inc) const;
  bool Empty() const { return n_blocks_ == 0; }
  double MinValue() const { return BlockMin(0); }
  double MaxValue() const { return BlockMax(n_blocks_ - 1); }

 private:
  struct BlockRange { uint32_t first_block, last_block; bool any; };
  BlockRange Overlap(double lo, bool lo_inc, double hi, bool hi_inc) const;
  double BlockMin(uint32_t b) const;
  double BlockMax(uint32_t b) const;
  uint32_t BlockCount(uint32_t b) const;
  const char* region_;
  uint32_t n_blocks_ = 0;
  const char* summaries_;  // n_blocks x 32-byte summary records
};

}  // namespace experiment::sai
