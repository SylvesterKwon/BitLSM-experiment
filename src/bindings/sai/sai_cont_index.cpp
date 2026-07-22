#include "sai_cont_index.h"
#include "sai_coding.h"
#include <algorithm>
#include <cassert>
#include <queue>

namespace experiment::sai {

// Summary record: [f64 min][f64 max][u32 count][u32 values_off][u32 perm_off]
// [u32 postings_off] = 32 bytes.
static constexpr uint32_t kSummaryBytes = 32;

void ContWriter::AppendTo(std::string& out) const {
  const size_t region_base = out.size();
  std::stable_sort(pts_.begin(), pts_.end(),
                   [](const Pt& a, const Pt& b) { return a.v < b.v; });
  const uint32_t n = static_cast<uint32_t>(pts_.size());
  const uint32_t nb = (n + kContBlockEntries - 1) / kContBlockEntries;
  PutU32(out, nb);
  const size_t summary_start = out.size();
  for (uint32_t b = 0; b < nb; ++b)
    for (int i = 0; i < 8; ++i) PutU32(out, 0);  // reserve 32B per block
  std::vector<std::pair<uint32_t, uint32_t>> rows_sorted;  // (rowId, value_pos)
  for (uint32_t b = 0; b < nb; ++b) {
    const uint32_t lo = b * kContBlockEntries;
    const uint32_t hi = std::min(n, lo + kContBlockEntries);
    const uint32_t cnt = hi - lo;
    char* srec = out.data() + summary_start + b * kSummaryBytes;
    std::memcpy(srec, &pts_[lo].v, 8);        // min (value-sorted)
    std::memcpy(srec + 8, &pts_[hi - 1].v, 8);  // max
    PatchU32(out, summary_start + b * kSummaryBytes + 16, cnt);
    // values area (value-sorted)
    PatchU32(out, summary_start + b * kSummaryBytes + 20,
             static_cast<uint32_t>(out.size() - region_base));
    for (uint32_t i = lo; i < hi; ++i) PutF64(out, pts_[i].v);
    // rowId-sorted view of this block: (rowId, value-order position within block)
    rows_sorted.clear();
    for (uint32_t i = lo; i < hi; ++i) rows_sorted.push_back({pts_[i].r, i - lo});
    std::sort(rows_sorted.begin(), rows_sorted.end());
    // perm: perm[value_pos] = posting ordinal (position in rowId-sorted list)
    std::vector<uint16_t> perm(cnt);
    for (uint32_t j = 0; j < cnt; ++j) perm[rows_sorted[j].second] = static_cast<uint16_t>(j);
    PatchU32(out, summary_start + b * kSummaryBytes + 24,
             static_cast<uint32_t>(out.size() - region_base));
    for (uint32_t i = 0; i < cnt; ++i) PutU16(out, perm[i]);
    // postings (ascending rowIds)
    PatchU32(out, summary_start + b * kSummaryBytes + 28,
             static_cast<uint32_t>(out.size() - region_base));
    PostingsBuilder pb;
    for (const auto& [r, vp] : rows_sorted) { (void)vp; pb.Add(r); }
    pb.AppendTo(out);
  }
}

ContReader::ContReader(const char* region) : region_(region) {
  n_blocks_ = GetU32(region_);
  summaries_ = region_ + 4;
}

double ContReader::BlockMin(uint32_t b) const { return GetF64(summaries_ + b * kSummaryBytes); }
double ContReader::BlockMax(uint32_t b) const { return GetF64(summaries_ + b * kSummaryBytes + 8); }
uint32_t ContReader::BlockCount(uint32_t b) const { return GetU32(summaries_ + b * kSummaryBytes + 16); }

ContReader::BlockRange ContReader::Overlap(double lo, bool lo_inc, double hi,
                                           bool hi_inc) const {
  BlockRange r{0, 0, false};
  if (n_blocks_ == 0) return r;
  auto block_overlaps = [&](uint32_t b) {
    const double mn = BlockMin(b), mx = BlockMax(b);
    const bool below = lo_inc ? (mx < lo) : (mx <= lo);
    const bool above = hi_inc ? (mn > hi) : (mn >= hi);
    return !(below || above);
  };
  uint32_t f = 0;
  while (f < n_blocks_ && !block_overlaps(f)) ++f;
  if (f == n_blocks_) return r;
  uint32_t l = n_blocks_ - 1;
  while (!block_overlaps(l)) --l;  // f exists, so this terminates
  r = {f, l, true};
  return r;
}

uint64_t ContReader::Estimate(double lo, bool lo_inc, double hi, bool hi_inc) const {
  const BlockRange br = Overlap(lo, lo_inc, hi, hi_inc);
  if (!br.any) return 0;
  uint64_t sum = 0;
  for (uint32_t b = br.first_block; b <= br.last_block; ++b) sum += BlockCount(b);
  return sum;  // boundary blocks counted whole -> upper bound (same nature as
               // Cassandra's matched-leaf posting sum)
}

namespace {
// One block's contribution: either whole postings (interior) or the subset of
// posting ordinals whose values fall in range (boundary), via the permutation.
class BlockCursor : public RowCursor {
 public:
  BlockCursor(const char* region, const char* srec, double lo, bool lo_inc,
              double hi, bool hi_inc, bool boundary) {
    const uint32_t cnt = GetU32(srec + 16);
    const char* values = region + GetU32(srec + 20);
    const char* perm = region + GetU32(srec + 24);
    const char* postings = region + GetU32(srec + 28);
    PostingsCursor pc(postings);
    if (!boundary) {
      rows_.reserve(cnt);
      while (pc.Valid()) { rows_.push_back(pc.Row()); pc.Next(); }
    } else {
      // Binary search the value-sorted array for [a, b) positions in range.
      auto val = [&](uint32_t i) { return GetF64(values + i * 8); };
      uint32_t a = 0, b = cnt;
      {  // first position satisfying the lower bound
        uint32_t loi = 0, hii = cnt;
        while (loi < hii) {
          uint32_t mid = (loi + hii) / 2;
          const bool ok = lo_inc ? (val(mid) >= lo) : (val(mid) > lo);
          if (ok) hii = mid; else loi = mid + 1;
        }
        a = loi;
      }
      {  // one past the last position satisfying the upper bound
        uint32_t loi = a, hii = cnt;
        while (loi < hii) {
          uint32_t mid = (loi + hii) / 2;
          const bool ok = hi_inc ? (val(mid) <= hi) : (val(mid) < hi);
          if (ok) loi = mid + 1; else hii = mid;
        }
        b = loi;
      }
      if (a >= b) return;
      // Collect the posting ordinals of in-range values, sort, pick from the
      // decoded posting list (LeafOrderMap-equivalent extraction).
      std::vector<uint16_t> ords;
      ords.reserve(b - a);
      for (uint32_t i = a; i < b; ++i) ords.push_back(GetU16(perm + i * 2));
      std::sort(ords.begin(), ords.end());
      std::vector<uint32_t> all;
      all.reserve(cnt);
      while (pc.Valid()) { all.push_back(pc.Row()); pc.Next(); }
      rows_.reserve(ords.size());
      for (uint16_t o : ords) rows_.push_back(all[o]);  // ascending (ords asc, all asc)
    }
  }
  bool Valid() const override { return pos_ < rows_.size(); }
  uint32_t Row() const override { return rows_[pos_]; }
  void Next() override { ++pos_; }
  void AdvanceTo(uint32_t t) override {
    pos_ = static_cast<uint32_t>(
        std::lower_bound(rows_.begin() + pos_, rows_.end(), t) - rows_.begin());
  }

 private:
  std::vector<uint32_t> rows_;
  uint32_t pos_ = 0;
};

// K-way min-heap merge over per-block cursors; rowIds are unique across blocks.
class MergedCursor : public RowCursor {
 public:
  explicit MergedCursor(std::vector<std::unique_ptr<BlockCursor>> blocks)
      : blocks_(std::move(blocks)) {
    for (auto& b : blocks_)
      if (b->Valid()) heap_.push(b.get());
  }
  bool Valid() const override { return !heap_.empty(); }
  uint32_t Row() const override { return heap_.top()->Row(); }
  void Next() override {
    BlockCursor* top = heap_.top();
    heap_.pop();
    top->Next();
    if (top->Valid()) heap_.push(top);
  }
  void AdvanceTo(uint32_t t) override {
    while (!heap_.empty() && heap_.top()->Row() < t) {
      BlockCursor* top = heap_.top();
      heap_.pop();
      top->AdvanceTo(t);
      if (top->Valid()) heap_.push(top);
    }
  }

 private:
  struct Cmp {
    bool operator()(const BlockCursor* a, const BlockCursor* b) const {
      return a->Row() > b->Row();
    }
  };
  std::vector<std::unique_ptr<BlockCursor>> blocks_;
  std::priority_queue<BlockCursor*, std::vector<BlockCursor*>, Cmp> heap_;
};
}  // namespace

std::unique_ptr<RowCursor> ContReader::OpenCursor(double lo, bool lo_inc,
                                                  double hi, bool hi_inc) const {
  const BlockRange br = Overlap(lo, lo_inc, hi, hi_inc);
  if (!br.any) return nullptr;
  std::vector<std::unique_ptr<BlockCursor>> blocks;
  for (uint32_t b = br.first_block; b <= br.last_block; ++b) {
    const char* srec = summaries_ + b * kSummaryBytes;
    // A block is interior when its whole [min,max] fits the query range.
    const double mn = GetF64(srec), mx = GetF64(srec + 8);
    const bool lo_ok = lo_inc ? (mn >= lo) : (mn > lo);
    const bool hi_ok = hi_inc ? (mx <= hi) : (mx < hi);
    const bool interior = lo_ok && hi_ok;
    blocks.push_back(std::make_unique<BlockCursor>(region_, srec, lo, lo_inc, hi,
                                                   hi_inc, !interior));
  }
  return std::make_unique<MergedCursor>(std::move(blocks));
}

}  // namespace experiment::sai
