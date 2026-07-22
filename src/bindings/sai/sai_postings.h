#pragma once
// Posting-list codec for the SAI baseline.
// Corresponds to Cassandra's PostingsWriter/PostingsReader
// (src/java/org/apache/cassandra/index/sai/disk/v1/postings/PostingsWriter.java:88-305):
// ascending rowIds, delta + FoR bit-packing in blocks of 128, block skip table of
// (offset, max). Simplified codec (u32 headers, plain skip table) — deviation D8.
// IntersectionCursor corresponds to KeyRangeIntersectionIterator.computeNext
// (iterators/KeyRangeIntersectionIterator.java:66-126).
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace experiment::sai {

inline constexpr uint32_t kPostingsBlockSize = 128;

struct RowCursor {
  virtual ~RowCursor() = default;
  virtual bool Valid() const = 0;
  virtual uint32_t Row() const = 0;
  virtual void Next() = 0;
  virtual void AdvanceTo(uint32_t t) = 0;  // first posting >= t
};

class PostingsBuilder {
 public:
  void Add(uint32_t row) {
    assert(rows_.empty() || row > rows_.back());
    rows_.push_back(row);
  }
  uint32_t Count() const { return static_cast<uint32_t>(rows_.size()); }
  void AppendTo(std::string& out) const;

 private:
  std::vector<uint32_t> rows_;
};

class PostingsCursor : public RowCursor {
 public:
  explicit PostingsCursor(const char* list_base);
  uint32_t Count() const { return n_; }
  bool Valid() const override { return valid_; }
  uint32_t Row() const override { return decoded_[pos_]; }
  void Next() override;
  void AdvanceTo(uint32_t t) override;

 private:
  void LoadBlock(uint32_t block_idx);  // decode block into decoded_
  const char* base_;
  uint32_t n_ = 0, nblocks_ = 0;
  const char* skip_;    // skip table start
  const char* blocks_;  // unused; block offsets come from the skip table
  uint32_t cur_block_ = 0;
  std::vector<uint32_t> decoded_;  // current block's postings
  uint32_t pos_ = 0;
  bool valid_ = false;
};

class IntersectionCursor : public RowCursor {
 public:
  explicit IntersectionCursor(std::vector<std::unique_ptr<RowCursor>> children);
  bool Valid() const override { return valid_; }
  uint32_t Row() const override { return current_; }
  void Next() override;
  void AdvanceTo(uint32_t t) override;

 private:
  void Align();  // advance-all-to-highest-key loop
  std::vector<std::unique_ptr<RowCursor>> children_;
  uint32_t current_ = 0;
  bool valid_ = false;
  bool primed_ = false;
};

}  // namespace experiment::sai
