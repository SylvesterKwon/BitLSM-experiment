#include "sai_ondemand.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <queue>

#include "sai_coding.h"

namespace experiment::sai {

namespace {
// Largest a serialized posting block can be: first rowId, the bits-per-value
// byte, and 127 deltas of at most 32 bits each.
constexpr uint32_t kMaxPostingBlockBytes = 4 + 1 + (127 * 32 + 7) / 8;
// Summary record: [f64 min][f64 max][u32 count][u32 values_off][u32 perm_off]
// [u32 postings_off].
constexpr uint32_t kSummaryBytes = 32;
}  // namespace

bool TrieLookupOnDemand(BlobSource& src, uint32_t trie_off,
                        std::string_view term, TrieEntry& e) {
  std::string buf;
  uint32_t node = trie_off + src.U32(trie_off);  // root
  for (unsigned char b : term) {
    const bool has_value = (src.Read(node, 1, buf)[0] & 1) != 0;
    // Skip the node's value to reach its child list.
    const uint32_t child_list = node + 1 + (has_value ? 8 : 0);
    const uint16_t nch = src.U16(child_list);
    if (nch == 0) return false;
    // The (byte, offset) pairs are sorted and the fan-out is small, so the
    // whole list comes back in one read.
    const char* children = src.Read(child_list + 2, nch * 5, buf);
    if (!src.ok()) return false;
    const char* found = nullptr;
    for (uint16_t i = 0; i < nch; ++i) {
      const uint8_t cb = static_cast<uint8_t>(children[i * 5]);
      if (cb == b) {
        found = children + i * 5 + 1;
        break;
      }
      if (cb > b) break;
    }
    if (!found) return false;
    node = trie_off + GetU32(found);
  }
  const char* leaf = src.Read(node, 9, buf);
  if (!src.ok() || (leaf[0] & 1) == 0) return false;
  e.postings_off = GetU32(leaf + 1);
  e.count = GetU32(leaf + 5);
  return true;
}

PostingsCursorOnDemand::PostingsCursorOnDemand(BlobSource* src,
                                               uint32_t list_off,
                                               uint32_t known_extent_len)
    : src_(src), list_off_(list_off) {
  skip_off_ = list_off_ + 8;
  if (known_extent_len != 0) {
    // The caller bounded the whole list extent from a directory it already
    // holds, so fetch FIRST and decode the header from the pinned bytes: a
    // cold open costs exactly one extent read (no page-granular header read
    // ahead of it), a cached one costs zero I/O.
    local_ = src_->FetchExtent(list_off_,
                               src_->Clamp(list_off_, known_extent_len));
    if (!src_->ok()) return;
    if (local_.size() >= 8) {
      local_ok_ = true;
      n_ = GetU32(local_.data());
      nblocks_ = GetU32(local_.data() + 4);
      if (n_ == 0) return;
      LoadBlock(0);
      valid_ = src_->ok();
      return;
    }
    local_.Release();  // cap 0 (empty pin): per-block fallback below
  }
  std::string buf;
  const char* head = src_->Read(list_off_, 8, buf);
  if (!src_->ok()) return;
  n_ = GetU32(head);
  nblocks_ = GetU32(head + 4);
  if (n_ == 0) return;
  // No caller-supplied bound: size the extent from the header and the last
  // skip entry just read. Its end is the last block's offset plus the
  // worst-case encoded block size (the exact size depends on that block's
  // bits-per-value, unknown until decoded); the clamp caps the over-read at
  // the end of the blob, whose trailing bytes are simply never decoded. The
  // extent length is fully determined by list_off_, so every open of this
  // list requests the identical extent -- on a FileBlobSource the first
  // open caches it and later opens pin it with zero I/O. An empty pin
  // (cap 0) falls back to per-block reads.
  const uint32_t last_rel = src_->U32(skip_off_ + (nblocks_ - 1) * 8);
  if (!src_->ok()) return;
  const uint64_t extent =
      static_cast<uint64_t>(last_rel) + kMaxPostingBlockBytes;
  if (AsyncPageBatch()) {
    // Page-granular mode drains this list one block at a time, so every page
    // of [header | skip table | blocks] is read before the cursor is done and
    // the whole thing can be submitted now. `extent` is tight -- it ends at
    // the LAST block's offset (out of the skip table just read) plus one
    // worst-case block -- so this batches exactly the pages the per-block
    // reads would have faulted in one at a time, no dead bytes beyond the
    // same bounded tail over-read those reads already take.
    const BlobRange r{list_off_,
                      src_->Clamp(list_off_, static_cast<uint32_t>(extent))};
    src_->PrefetchRanges(&r, 1);
  }
  local_ = src_->FetchExtent(
      list_off_, src_->Clamp(list_off_, static_cast<uint32_t>(extent)));
  if (!src_->ok()) return;
  local_ok_ = local_.data() != nullptr;
  LoadBlock(0);
  valid_ = src_->ok();
}

uint32_t PostingsCursorOnDemand::SkipU32(uint32_t rel_to_list) {
  // local_.data() serves both pin flavors (cache entry or owned buffer).
  return local_ok_ ? GetU32(local_.data() + rel_to_list)
                   : src_->U32(list_off_ + rel_to_list);
}

void PostingsCursorOnDemand::LoadBlock(uint32_t block_idx) {
  cur_block_ = block_idx;
  const uint32_t rel_off = SkipU32(8 + block_idx * 8);
  const uint32_t lo = block_idx * kPostingsBlockSize;
  const uint32_t cnt = std::min(n_, lo + kPostingsBlockSize) - lo;
  const char* p;
  if (local_ok_) {
    // The pinned extent holds the whole list; the decode below walks exactly
    // the block's encoded bytes, all inside it.
    p = local_.data() + rel_off;
  } else {
    // Per-block fallback: the encoded length depends on the block's
    // bits-per-value, which is only known once the block is read, so take the
    // worst case and let the source clamp it against the end of the blob.
    const uint32_t block_off = list_off_ + rel_off;
    p = src_->Read(block_off, src_->Clamp(block_off, kMaxPostingBlockBytes),
                   scratch_);
  }
  if (!src_->ok()) {
    valid_ = false;
    return;
  }
  decoded_.resize(cnt);
  decoded_[0] = GetU32(p);
  p += 4;
  const uint8_t bpv = static_cast<uint8_t>(*p);
  ++p;
  uint64_t acc = 0;
  uint32_t acc_bits = 0;
  const uint32_t mask_bits = bpv;
  for (uint32_t i = 1; i < cnt; ++i) {
    while (acc_bits < mask_bits) {
      acc |= static_cast<uint64_t>(static_cast<uint8_t>(*p)) << acc_bits;
      ++p;
      acc_bits += 8;
    }
    const uint32_t d = static_cast<uint32_t>(acc & ((1ull << mask_bits) - 1));
    acc >>= mask_bits;
    acc_bits -= mask_bits;
    decoded_[i] = decoded_[i - 1] + d;
  }
  pos_ = 0;
}

void PostingsCursorOnDemand::Next() {
  ++pos_;
  if (pos_ < decoded_.size()) return;
  if (cur_block_ + 1 < nblocks_) {
    LoadBlock(cur_block_ + 1);
    valid_ = src_->ok();
  } else {
    valid_ = false;
  }
}

void PostingsCursorOnDemand::AdvanceTo(uint32_t t) {
  if (!valid_ || Row() >= t) return;
  // Walk the skip table for the first block whose max reaches t. The probes
  // hit the local list buffer; only the per-block fallback reads through the
  // source (a 4-byte read each; a page holds 512 of them).
  uint32_t b = cur_block_;
  while (b < nblocks_ && SkipU32(8 + b * 8 + 4) < t) ++b;
  if (b >= nblocks_ || !src_->ok()) {
    valid_ = false;
    return;
  }
  if (b != cur_block_) {
    LoadBlock(b);
    if (!src_->ok()) {
      valid_ = false;
      return;
    }
    pos_ = 0;
  }
  auto it = std::lower_bound(decoded_.begin() + pos_, decoded_.end(), t);
  pos_ = static_cast<uint32_t>(it - decoded_.begin());
}

ContReaderOnDemand::ContReaderOnDemand(BlobSource* src, uint32_t region_off)
    : src_(src), region_off_(region_off) {
  n_blocks_ = src_->U32(region_off_);
  summaries_off_ = region_off_ + 4;
  // Pin the whole summary array as one extent: Overlap()'s scans and
  // Estimate()'s per-block counts otherwise pay one cache round trip per
  // 8-byte probe. The uint32 guard keeps the Clamp cast exact for a
  // corrupt/absurd n_blocks_; the Clamp check keeps a truncated blob on the
  // per-probe path (whose reads then fail cleanly) instead of fetching a
  // short extent.
  const uint64_t want = static_cast<uint64_t>(n_blocks_) * kSummaryBytes;
  if (src_->ok() && n_blocks_ > 0 &&
      want <= std::numeric_limits<uint32_t>::max() &&
      src_->Clamp(summaries_off_, static_cast<uint32_t>(want)) == want) {
    summaries_ =
        src_->FetchExtent(summaries_off_, static_cast<uint32_t>(want));
    summaries_ok_ = summaries_.data() != nullptr;
    if (!summaries_ok_ && AsyncPageBatch()) {
      // Cap 0: no summary extent, so Overlap() and the per-leaf walk probe
      // the array 8 bytes at a time through the source. Overlap scans from
      // block 0 forward and from block n-1 backward and OpenCursor then
      // walks everything between, so the WHOLE array is read either way --
      // one batched submission instead of one pread per 4 KB page, over
      // byte-identical bytes.
      const BlobRange r{summaries_off_, static_cast<uint32_t>(want)};
      src_->PrefetchRanges(&r, 1);
    }
  }
}

double ContReaderOnDemand::BlockMin(uint32_t b) {
  return summaries_ok_ ? GetF64(summaries_.data() + b * kSummaryBytes)
                       : src_->F64(summaries_off_ + b * kSummaryBytes);
}
double ContReaderOnDemand::BlockMax(uint32_t b) {
  return summaries_ok_ ? GetF64(summaries_.data() + b * kSummaryBytes + 8)
                       : src_->F64(summaries_off_ + b * kSummaryBytes + 8);
}
uint32_t ContReaderOnDemand::BlockCount(uint32_t b) {
  return summaries_ok_ ? GetU32(summaries_.data() + b * kSummaryBytes + 16)
                       : src_->U32(summaries_off_ + b * kSummaryBytes + 16);
}
uint32_t ContReaderOnDemand::ValuesOff(uint32_t b) {
  return summaries_ok_ ? GetU32(summaries_.data() + b * kSummaryBytes + 20)
                       : src_->U32(summaries_off_ + b * kSummaryBytes + 20);
}
const char* ContReaderOnDemand::SummaryRec(uint32_t b, std::string& buf) {
  if (summaries_ok_) return summaries_.data() + b * kSummaryBytes;
  return src_->Read(summaries_off_ + b * kSummaryBytes, kSummaryBytes, buf);
}

ContReaderOnDemand::BlockRange ContReaderOnDemand::Overlap(double lo,
                                                           bool lo_inc,
                                                           double hi,
                                                           bool hi_inc) {
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
  // f exists, so this terminates -- unless a blob read that succeeded during
  // the forward scan fails on retry here (a genuine I/O error, not just a
  // cache miss); guard the underflow so a flaky read degrades to "no match"
  // instead of wrapping `l` and returning a garbage block range.
  while (!block_overlaps(l)) {
    if (l == 0) return r;
    --l;
  }
  r = {f, l, true};
  return r;
}

uint64_t ContReaderOnDemand::Estimate(double lo, bool lo_inc, double hi,
                                      bool hi_inc) {
  const BlockRange br = Overlap(lo, lo_inc, hi, hi_inc);
  if (!br.any) return 0;
  uint64_t sum = 0;
  for (uint32_t b = br.first_block; b <= br.last_block; ++b) sum += BlockCount(b);
  return sum;  // boundary blocks counted whole, matching ContReader
}

namespace {
// One leaf's contribution, materialised the same way ContReader's BlockCursor
// does it: whole postings for an interior leaf, or the postings whose values
// fall in range for a boundary leaf. `srec` is the leaf's 32-byte summary
// record (the caller already holds the summary array). For a BOUNDARY leaf,
// `src` is normally a WindowBlobSource pinning that leaf's whole record, so
// the values binary search and perm read land in the pinned bytes (and the
// postings cursor's FetchExtent resolves to a local copy out of the window,
// not a second overlapping cache entry); an interior leaf reads only its
// postings, which PostingsCursorOnDemand fetches by itself, so it needs no
// window.
class BlockCursorOnDemand : public RowCursor {
 public:
  BlockCursorOnDemand(BlobSource* src, uint32_t region_off, const char* srec,
                      double lo, bool lo_inc, double hi, bool hi_inc,
                      bool boundary, uint32_t postings_extent_len) {
    const uint32_t cnt = GetU32(srec + 16);
    const uint32_t values_off = region_off + GetU32(srec + 20);
    const uint32_t perm_off = region_off + GetU32(srec + 24);
    const uint32_t postings_off = region_off + GetU32(srec + 28);

    PostingsCursorOnDemand pc(src, postings_off, postings_extent_len);
    if (!boundary) {
      rows_.reserve(cnt);
      while (pc.Valid()) {
        rows_.push_back(pc.Row());
        pc.Next();
      }
      return;
    }
    // Binary search the value-sorted array for the in-range positions. Each
    // probe is an 8-byte read; a leaf's values span at most two pages.
    auto val = [&](uint32_t i) { return src->F64(values_off + i * 8); };
    uint32_t a = 0, b = cnt;
    {
      uint32_t loi = 0, hii = cnt;
      while (loi < hii) {
        uint32_t mid = (loi + hii) / 2;
        const bool ok = lo_inc ? (val(mid) >= lo) : (val(mid) > lo);
        if (ok) hii = mid; else loi = mid + 1;
      }
      a = loi;
    }
    {
      uint32_t loi = a, hii = cnt;
      while (loi < hii) {
        uint32_t mid = (loi + hii) / 2;
        const bool ok = hi_inc ? (val(mid) <= hi) : (val(mid) < hi);
        if (ok) loi = mid + 1; else hii = mid;
      }
      b = loi;
    }
    if (a >= b) return;
    std::string perm_buf;
    const char* perm = src->Read(perm_off + a * 2, (b - a) * 2, perm_buf);
    if (!src->ok()) return;
    std::vector<uint16_t> ords;
    ords.reserve(b - a);
    for (uint32_t i = 0; i < b - a; ++i) ords.push_back(GetU16(perm + i * 2));
    std::sort(ords.begin(), ords.end());
    std::vector<uint32_t> all;
    all.reserve(cnt);
    while (pc.Valid()) {
      all.push_back(pc.Row());
      pc.Next();
    }
    rows_.reserve(ords.size());
    for (uint16_t o : ords) rows_.push_back(all[o]);  // ords asc, all asc
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

// K-way min-heap merge over leaf cursors; rowIds are unique across leaves.
class MergedCursorOnDemand : public RowCursor {
 public:
  explicit MergedCursorOnDemand(
      std::vector<std::unique_ptr<BlockCursorOnDemand>> blocks)
      : blocks_(std::move(blocks)) {
    for (auto& b : blocks_)
      if (b->Valid()) heap_.push(b.get());
  }
  bool Valid() const override { return !heap_.empty(); }
  uint32_t Row() const override { return heap_.top()->Row(); }
  void Next() override {
    BlockCursorOnDemand* top = heap_.top();
    heap_.pop();
    top->Next();
    if (top->Valid()) heap_.push(top);
  }
  void AdvanceTo(uint32_t t) override {
    while (!heap_.empty() && heap_.top()->Row() < t) {
      BlockCursorOnDemand* top = heap_.top();
      heap_.pop();
      top->AdvanceTo(t);
      if (top->Valid()) heap_.push(top);
    }
  }

 private:
  struct Cmp {
    bool operator()(const BlockCursorOnDemand* a,
                    const BlockCursorOnDemand* b) const {
      return a->Row() > b->Row();
    }
  };
  std::vector<std::unique_ptr<BlockCursorOnDemand>> blocks_;
  std::priority_queue<BlockCursorOnDemand*, std::vector<BlockCursorOnDemand*>, Cmp>
      heap_;
};
}  // namespace

std::unique_ptr<RowCursor> ContReaderOnDemand::OpenCursor(double lo, bool lo_inc,
                                                          double hi,
                                                          bool hi_inc) {
  const BlockRange br = Overlap(lo, lo_inc, hi, hi_inc);
  if (!br.any) return nullptr;
  std::vector<std::unique_ptr<BlockCursorOnDemand>> blocks;
  std::string sbuf;
  // A leaf is interior when its whole [min, max] fits the query range.
  // Leaves are value-sorted and non-overlapping, so only the range's two
  // END leaves can be boundary; everything strictly between is interior.
  auto leaf_interior = [&](const char* srec) {
    const double mn = GetF64(srec), mx = GetF64(srec + 8);
    const bool lo_ok = lo_inc ? (mn >= lo) : (mn > lo);
    const bool hi_ok = hi_inc ? (mx <= hi) : (mx < hi);
    return lo_ok && hi_ok;
  };
  // The postings byte-ranges of a whole numeric range are known here, before
  // a single one of them is read: the leaf set is [first_block, last_block]
  // and each leaf's postings run from its own postings offset to the next
  // leaf's values offset (records are values|perm|postings back to back), all
  // out of the summary array alone. In page-granular mode those become one to
  // three scattered 4 KB preads PER LEAF, issued one at a time as each leaf
  // cursor opens -- the request count the coarse per-range extent buys its
  // dead bytes to avoid. Handing the whole set to the source lets it submit
  // them together (FileBlobSource::PrefetchRanges -> MultiRead -> io_uring),
  // which is what BitLSM's cold bin runs already do
  // (BitLSM src/include/block_prefetch_queue.h). The batch fetches the pages
  // the per-leaf reads would have faulted in, minus the ones the cache
  // already holds -- read granularity, page keys and cross-query reuse are
  // untouched, only the submission changes.
  //
  // An INTERIOR leaf is asked for its postings only. A BOUNDARY leaf (at most
  // the range's two ends) also binary-searches its values array and reads a
  // permutation slice, so it is asked for its whole values|perm|postings
  // record. Both ends carry the same kMaxPostingBlockBytes tail slack the
  // per-block reads take. The only pages this can add over the serial path
  // are the ones inside a boundary leaf's values|perm that its binary search
  // happens to skip -- at most two leaves per range.
  if (AsyncPageBatch()) {
    std::vector<BlobRange> want;
    want.reserve(br.last_block - br.first_block + 1);
    for (uint32_t b = br.first_block; b <= br.last_block; ++b) {
      const char* srec = SummaryRec(b, sbuf);
      if (!src_->ok()) return nullptr;
      const uint32_t begin_rel =
          leaf_interior(srec) ? GetU32(srec + 28) : GetU32(srec + 20);
      uint64_t end_rel;
      if (b + 1 < n_blocks_) {
        end_rel = static_cast<uint64_t>(ValuesOff(b + 1)) +
                  kMaxPostingBlockBytes;
        if (!src_->ok()) return nullptr;
      } else {
        // Region's last leaf: no next record to end on, so bound its postings
        // the same way the boundary windows do -- header, skip table and a
        // worst-case block each.
        const uint32_t cnt = GetU32(srec + 16);
        const uint32_t nbk =
            (cnt + kPostingsBlockSize - 1) / kPostingsBlockSize;
        end_rel = static_cast<uint64_t>(GetU32(srec + 28)) + 8 +
                  static_cast<uint64_t>(nbk) * (8 + kMaxPostingBlockBytes);
      }
      if (end_rel <= begin_rel ||
          end_rel - begin_rel > std::numeric_limits<uint32_t>::max()) {
        continue;  // corrupt/absurd directory: skip the hint, read as usual
      }
      const uint32_t off = region_off_ + begin_rel;
      want.push_back(
          {off, src_->Clamp(off, static_cast<uint32_t>(end_rel - begin_rel))});
    }
    if (!want.empty()) src_->PrefetchRanges(want.data(), want.size());
  }
  // The interior leaves therefore form one contiguous run [fi, li_end):
  // trimming a boundary end off each side of [first_block, last_block]
  // leaves exactly the interior set. Their postings are fetched as ONE
  // per-range extent -- the byte span from the first interior leaf's
  // postings to the last one's end (kMax slack for the bounded over-read,
  // same rationale as the boundary window) -- pinned once and shared by
  // every interior cursor, which decodes its own list out of the pinned
  // bytes. This is the fair mirror of BitLSM's BinRange/LoadRun: one
  // contiguous read per (attr, SST, query range) on the cold path instead
  // of one tiny read per leaf. The span interleaves the interior leaves'
  // values|perm between their postings (records are values|perm|postings
  // back to back), so the coarse read carries dead bytes -- the accepted
  // price of coarsening. The CACHE holds one entry per span, keyed at the
  // span's start offset: two queries reuse it only when their ranges share
  // a first interior leaf; a longer cached span serves a shorter request,
  // a shorter one is reloaded and replaced (FetchExtent's length guard).
  std::unique_ptr<WindowBlobSource> range_win;
  if (LocalExtentCap() != 0) {
    uint32_t fi = br.first_block;
    uint32_t li_end = br.last_block + 1;  // exclusive
    {
      const char* fs = SummaryRec(fi, sbuf);
      if (!src_->ok()) return nullptr;
      if (!leaf_interior(fs)) ++fi;
    }
    if (fi < li_end) {
      const char* ls = SummaryRec(li_end - 1, sbuf);
      if (!src_->ok()) return nullptr;
      if (!leaf_interior(ls)) --li_end;
    }
    if (fi < li_end) {
      const char* fs = SummaryRec(fi, sbuf);
      if (!src_->ok()) return nullptr;
      const uint32_t span_begin_rel = GetU32(fs + 28);
      const uint32_t last = li_end - 1;
      uint64_t span_end_rel;
      if (last + 1 < n_blocks_) {
        span_end_rel = static_cast<uint64_t>(ValuesOff(last + 1)) +
                       kMaxPostingBlockBytes;
      } else {
        const char* ls = SummaryRec(last, sbuf);
        if (!src_->ok()) return nullptr;
        const uint32_t cnt = GetU32(ls + 16);
        const uint32_t nbk =
            (cnt + kPostingsBlockSize - 1) / kPostingsBlockSize;
        span_end_rel = static_cast<uint64_t>(GetU32(ls + 28)) + 8 +
                       static_cast<uint64_t>(nbk) *
                           (8 + kMaxPostingBlockBytes);
      }
      if (!src_->ok()) return nullptr;
      if (span_end_rel > span_begin_rel &&
          span_end_rel - span_begin_rel <=
              std::numeric_limits<uint32_t>::max()) {
        const uint32_t span_len = src_->Clamp(
            region_off_ + span_begin_rel,
            static_cast<uint32_t>(span_end_rel - span_begin_rel));
        range_win = std::make_unique<WindowBlobSource>(
            src_, region_off_ + span_begin_rel, span_len);
        if (!range_win->ok()) return nullptr;  // read failed; the caller's
                                               // FailIfError sees it on src_
      }
    }
  }
  for (uint32_t b = br.first_block; b <= br.last_block; ++b) {
    const char* srec = SummaryRec(b, sbuf);
    if (!src_->ok()) return nullptr;
    const bool interior = leaf_interior(srec);
    // The leaf's postings extent is known from the summary array alone:
    // records are contiguous, so leaf b's postings end exactly where leaf
    // b+1's values begin (worst-case-bounded for the region's last leaf,
    // clamped by the fetch). Handing the bound to the postings cursor lets
    // it fetch its extent FIRST and decode the header from the pinned bytes
    // -- without it, every cold interior leaf would pay a page-granular
    // header read for bytes its extent is about to fetch anyway (the
    // dominant page traffic of a numeric range: one 4 KB page per ~2 KB
    // list).
    uint32_t plen = 0;
    if (LocalExtentCap() != 0) {  // cap 0: the bound would go unused
      const uint32_t postings_rel = GetU32(srec + 28);
      uint64_t pend;
      if (b + 1 < n_blocks_) {
        pend = ValuesOff(b + 1);
        if (!src_->ok()) return nullptr;
      } else {
        const uint32_t cnt = GetU32(srec + 16);
        const uint32_t nbk =
            (cnt + kPostingsBlockSize - 1) / kPostingsBlockSize;
        pend = static_cast<uint64_t>(postings_rel) + 8 +
               static_cast<uint64_t>(nbk) * (8 + kMaxPostingBlockBytes);
      }
      if (pend > postings_rel &&
          pend - postings_rel <= std::numeric_limits<uint32_t>::max()) {
        plen = static_cast<uint32_t>(pend - postings_rel);
      }
    }
    // An interior leaf reads ONLY its postings, out of the shared per-range
    // extent pinned above (its postings cursor's FetchExtent resolves to a
    // local copy from the range window, never a second cache entry). A
    // boundary leaf also binary-searches the values array and reads a perm
    // slice, so it gets its own pinned window over its whole
    // values|perm|postings record: from its values offset to the next
    // leaf's values offset plus one worst-case posting block of slack (the
    // postings cursor's bounded over-read), or for the region's last leaf
    // to a worst-case bound on its postings size (cnt <= 1024 entries ->
    // at most 8 blocks); both clamped to the blob. The bounds are fully
    // determined by the leaf, so every query pins the identical extent --
    // cross-query reuse via the cache. Cap 0 skips both windows
    // (FetchExtent would decline anyway): per-block reads throughout,
    // byte-identical to the pre-extent path.
    std::unique_ptr<WindowBlobSource> win;
    BlobSource* rd = src_;
    if (interior && range_win != nullptr) {
      rd = range_win.get();
    } else if (!interior && LocalExtentCap() != 0) {
      const uint32_t span_begin = region_off_ + GetU32(srec + 20);
      uint64_t span_end;
      if (b + 1 < n_blocks_) {
        span_end = static_cast<uint64_t>(region_off_) + ValuesOff(b + 1) +
                   kMaxPostingBlockBytes;
      } else {
        const uint32_t cnt = GetU32(srec + 16);
        const uint32_t nb =
            (cnt + kPostingsBlockSize - 1) / kPostingsBlockSize;
        span_end = static_cast<uint64_t>(region_off_) + GetU32(srec + 28) + 8 +
                   static_cast<uint64_t>(nb) * (8 + kMaxPostingBlockBytes);
      }
      if (!src_->ok()) return nullptr;
      if (span_end > span_begin &&
          span_end - span_begin <= std::numeric_limits<uint32_t>::max()) {
        const uint32_t span_len = src_->Clamp(
            span_begin, static_cast<uint32_t>(span_end - span_begin));
        win = std::make_unique<WindowBlobSource>(src_, span_begin, span_len);
        if (!win->ok()) return nullptr;  // read failed; the caller's
                                         // FailIfError sees it on src_
        rd = win.get();
      }
    }
    blocks.push_back(std::make_unique<BlockCursorOnDemand>(
        rd, region_off_, srec, lo, lo_inc, hi, hi_inc, !interior, plen));
  }
  return std::make_unique<MergedCursorOnDemand>(std::move(blocks));
}

uint64_t OnDemandEstimate(BlobSource& src,
                          const std::vector<uint32_t>& region_off,
                          const SAIFact& f) {
  const uint32_t region = region_off[f.attr_idx];
  if (f.is_cat) {
    TrieEntry e;
    const uint32_t trie_off = region + src.U32(region);
    if (!TrieLookupOnDemand(src, trie_off, f.cat_value, e)) return 0;
    return e.count;
  }
  return ContReaderOnDemand(&src, region).Estimate(f.lo, f.lo_inc, f.hi,
                                                   f.hi_inc);
}

std::unique_ptr<RowCursor> OnDemandOpenCursor(
    BlobSource* src, const std::vector<uint32_t>& region_off,
    const SAIFact& f) {
  const uint32_t region = region_off[f.attr_idx];
  if (f.is_cat) {
    TrieEntry e;
    const uint32_t trie_off = region + src->U32(region);
    if (!TrieLookupOnDemand(*src, trie_off, f.cat_value, e)) return nullptr;
    return std::make_unique<PostingsCursorOnDemand>(src, region + e.postings_off);
  }
  return ContReaderOnDemand(src, region)
      .OpenCursor(f.lo, f.lo_inc, f.hi, f.hi_inc);
}

}  // namespace experiment::sai
