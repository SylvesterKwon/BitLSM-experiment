// Corresponds to BitLSM's SABITableIterator
// (third_party/BitLSM/src/include/bit_lsm_iterator.h); block-pruning predicate
// is BF+raw zone map instead of a bitmap.
//
// SABI selects exact ROW indexes from a bitmap; this iterator selects candidate
// BLOCKS from BF/zonemap and then scans EVERY entry in each candidate block,
// applying EmbeddedCodec::Evaluate exactly. The RocksDB-internal mechanics of
// opening a data-block iterator (GetUserDefinedIndexReader(),
// NewDataBlockIterator<DataBlockIter>) are ported from
// third_party/BitLSM/src/include/sabi_table_iterator.cpp.

#include <bit_lsm_query.h>

#include <cstdint>
#include <iostream>
#include <limits>

#include "rocksdb/options.h"
#include "table/block_based/block.h"
#include "table/format.h"
#define TEST_CACHE_LINE_SIZE \
  64  // To avoid compile error when including block_based_table_reader.h
      // together with other internal headers (matches sabi_table_iterator.cpp).

#include "embedded_iterator.h"
#include "embedded_value_codec.h"
#include "embedded_bloom.h"
#include "embedded_index.h"
#include "table/block_based/block_based_table_reader.h"
#include "table/block_based/block_based_table_reader_impl.h"  // Required: provides NewDataBlockIterator<> template definition
#include "util/coding.h"  // RocksDB internal: DecodeFixed32/DecodeFixed64

namespace experiment::embedded {

using namespace rocksdb;

static double U2D(uint64_t u) {
  double d;
  std::memcpy(&d, &u, sizeof(double));
  return d;
}

EmbeddedTableIterator::EmbeddedTableIterator(BlockBasedTable* bbt,
                                             bit_lsm::BitLSMOptions options,
                                             bit_lsm::BitLSMQuery query,
                                             bool track_source_versions,
                                             int source_level,
                                             uint64_t file_number,
                                             bool source_has_range_del)
    : options_(std::move(options)),
      bbt_(bbt),
      query_(std::move(query)),
      block_prefetcher_(
          /*compaction_readahead_size=*/0,
          bbt->get_rep()->table_options.initial_auto_readahead_size),
      track_source_versions_(track_source_versions),
      source_level_(source_level),
      file_number_(file_number),
      source_has_range_del_(source_has_range_del),
      prefetch_queue_(bbt, options_.scan_prefetch_depth) {
  // Holds the index entry for this iterator's lifetime: a block cache pin
  // when cache_index_and_filter_blocks is on (evictable after release), or an
  // unowned reference to the table-lifetime pin in Rep when off. Mirrors
  // SABITableIterator (third_party/BitLSM/src/include/sabi_table_iterator.cpp).
  Status s = bbt_->GetUserDefinedIndexReader(ReadOptions(), &udi_entry_);
  if (!s.ok()) {
    // Every failure here is fatal for the scan, NotFound (an SST carrying no
    // index block) included: the query path has no fallback scan, so skipping
    // this file would silently drop every row it holds. Record the status so
    // the parent iterators stop instead of reading it as "no matching rows".
    std::cerr << "Failed to load embedded index: " << s.ToString() << "\n";
    status_ = s;
    return;  // stays !Valid(); SeekToFirst is a no-op
  }
  idx_ = static_cast<EmbeddedIndexReader*>(udi_entry_.GetValue()->reader());

  SelectCandidateBlocks();
}

void EmbeddedTableIterator::SelectCandidateBlocks() {
  candidate_blocks_.clear();

  // --- Build flat-AND query facts from clause_groups. FLAT AND ONLY. ---
  // A clause with exactly one condition prunes; OR clauses (size > 1) and any
  // other shape DO NOT prune (skipped here, enforced by exact recheck later).
  struct CatFact {
    uint32_t attr_idx;
    std::string value;
  };
  struct ContFact {
    uint32_t attr_idx;
    uint32_t cont_ordinal;
    double lo;
    bool lo_inc;
    double hi;
    bool hi_inc;
  };
  std::vector<CatFact> cat_facts;
  std::vector<ContFact> cont_facts;

  const double NEG_INF = -std::numeric_limits<double>::infinity();
  const double POS_INF = std::numeric_limits<double>::infinity();

  // Map attr_idx -> index into cont_facts (coalesce per attr).
  auto find_cont = [&](uint32_t attr_idx) -> ContFact* {
    for (auto& f : cont_facts)
      if (f.attr_idx == attr_idx) return &f;
    return nullptr;
  };
  // cont_ordinal of an attr = number of ORDERED attrs with index < attr_idx
  // (matches the builder's file-level zone-map ordering).
  auto cont_ordinal_of = [&](uint32_t attr_idx) -> uint32_t {
    uint32_t ord = 0;
    for (uint32_t i = 0; i < attr_idx; ++i)
      if (options_.attr_specs[i].role == bit_lsm::AttrRole::ORDERED) ++ord;
    return ord;
  };

  for (const auto& clause : query_.clause_groups) {
    if (clause.size() != 1) continue;  // OR / non-single shapes do not prune.
    const bit_lsm::QueryCondition& c = clause[0];
    if (c.attr_idx >= options_.attr_specs.size()) continue;
    if (options_.attr_specs[c.attr_idx].role == bit_lsm::AttrRole::UNORDERED) {
      // Single categorical EQUAL condition only.
      if (c.op != bit_lsm::CompareOp::EQUAL) continue;
      cat_facts.push_back({c.attr_idx, std::get<std::string>(c.value)});
    } else {
      // Single continuous condition; coalesce into [lo, hi] per attr.
      double v = std::get<double>(c.value);
      ContFact* f = find_cont(c.attr_idx);
      if (f == nullptr) {
        cont_facts.push_back({c.attr_idx, cont_ordinal_of(c.attr_idx), NEG_INF,
                              true, POS_INF, true});
        f = &cont_facts.back();
      }
      switch (c.op) {
        case bit_lsm::CompareOp::EQUAL:
          f->lo = v;
          f->lo_inc = true;
          f->hi = v;
          f->hi_inc = true;
          break;
        case bit_lsm::CompareOp::GREATER_EQUAL:
          f->lo = v;
          f->lo_inc = true;
          break;
        case bit_lsm::CompareOp::GREATER:
          f->lo = v;
          f->lo_inc = false;
          break;
        case bit_lsm::CompareOp::LESS_EQUAL:
          f->hi = v;
          f->hi_inc = true;
          break;
        case bit_lsm::CompareOp::LESS:
          f->hi = v;
          f->hi_inc = false;
          break;
      }
    }
  }

  // --- FILE-LEVEL SKIP FIRST: if any continuous fact misses the file zone
  // map, no block in this file can match -> leave candidate_blocks_ empty. ---
  for (const auto& f : cont_facts) {
    if (!idx_->FileZoneOverlaps(f.cont_ordinal, f.lo, f.lo_inc, f.hi,
                                f.hi_inc)) {
      return;
    }
  }

  // --- Per-block: decode Section C in index order, test each fact. ---
  // Ondemand mode reads Section C through one FileBlobSource shared across
  // the whole loop, with the entire Section C span prefetched up front by a
  // WindowBlobSource: the sweep below touches every block's payload and the
  // section is one contiguous blob range, so the bulk read replaces a cache
  // lookup+memcpy per block (and one pread per missing page) with grouped
  // preads and local copies; the span read still populates the per-page
  // cache, so cross-query reuse is unchanged. Sections over the sanity cap
  // (unreachable at this workload's ~MB sections) degrade to the original
  // per-page reads. Resident mode dereferences the owned blob copy directly.
  // A read failure has no status channel through BlockFilterRegion's return
  // value, so it is recorded on the source and checked right after each read,
  // before any byte of that block's region is interpreted (same stance as the
  // SAI baseline's FailIfError: a silently-pruned block would under-return
  // rows, not just mis-time them).
  const uint32_t B = static_cast<uint32_t>(idx_->block_handles.size());
  std::unique_ptr<BlobSource> src;
  std::unique_ptr<WindowBlobSource> win;
  BlobSource* rd = nullptr;
  std::string region_scratch;
  if (idx_->MetadataOnly()) {
    src = idx_->MakeBlobSource();
    rd = src.get();
    if (B > 0) {
      const uint32_t c_begin = idx_->SectionCBegin();
      const uint32_t c_len = idx_->SectionCEnd() - c_begin;
      if (c_len <= kMaxLocalExtentBytes) {
        win = std::make_unique<WindowBlobSource>(src.get(), c_begin, c_len);
        if (!win->ok()) {
          status_ = Status::IOError(
              "EmbeddedTableIterator: ondemand Section C span read failed");
          candidate_blocks_.clear();
          return;
        }
        rd = win.get();
      }
    }
  }

  for (uint32_t bi = 0; bi < B; ++bi) {
    size_t region_len = 0;
    const char* region =
        rd ? idx_->ReadBlockFilterRegion(bi, *rd, region_scratch, region_len)
           : idx_->BlockFilterRegion(bi, region_len);
    if (rd && !rd->ok()) {
      // A failed read can leave `region_scratch` holding stale bytes from an
      // earlier iteration (std::string::resize keeps existing bytes when the
      // new size does not grow it); parsing that as nbits/zonemap fields
      // could walk far past the buffer. Stop before interpreting anything.
      status_ = Status::IOError(
          "EmbeddedTableIterator: ondemand filter-region read failed");
      candidate_blocks_.clear();
      return;
    }
    const char* p = region;

    bool block_ok = true;
    // Walk attrs in index order, advancing the read cursor regardless of
    // whether this attr has a query fact, so offsets stay aligned.
    for (uint32_t ai = 0; ai < options_.attr_specs.size() && block_ok; ++ai) {
      if (options_.attr_specs[ai].role == bit_lsm::AttrRole::UNORDERED) {
        // [u32 nbits][ceil(nbits/8) bytes]
        uint32_t nbits = DecodeFixed32(p);
        p += sizeof(uint32_t);
        const char* bloom_bytes = p;
        uint32_t nbytes = (nbits + 7) / 8;
        p += nbytes;
        // Test every categorical fact for this attr.
        for (const auto& cf : cat_facts) {
          if (cf.attr_idx != ai) continue;
          if (!BloomMaybe(bloom_bytes, nbits, idx_->bloom_bits, cf.value)) {
            block_ok = false;
            break;
          }
        }
      } else {
        // [u64 min_bits][u64 max_bits]
        double mn = U2D(DecodeFixed64(p));
        p += sizeof(uint64_t);
        double mx = U2D(DecodeFixed64(p));
        p += sizeof(uint64_t);
        ZoneMap z{mn, mx};
        for (const auto& cf : cont_facts) {
          if (cf.attr_idx != ai) continue;
          if (!z.overlaps(cf.lo, cf.lo_inc, cf.hi, cf.hi_inc)) {
            block_ok = false;
            break;
          }
        }
      }
    }
    (void)region_len;  // length is implied by the index-order walk.

    if (block_ok) {
      // Convert plain-field UDI BlockHandle -> rocksdb::BlockHandle.
      BlockHandle bh;
      bh.set_offset(idx_->block_handles[bi].offset);
      bh.set_size(idx_->block_handles[bi].size);
      candidate_blocks_.push_back(bh);
    }
  }

  // The candidate list is complete here, before the first block is read, so
  // the queue can start filling straight away.
  prefetch_targets_.reserve(candidate_blocks_.size());
  for (size_t i = 0; i < candidate_blocks_.size(); ++i)
    prefetch_targets_.push_back({static_cast<uint32_t>(i), candidate_blocks_[i]});
  prefetch_queue_.Prepare(&prefetch_targets_);
}

void EmbeddedTableIterator::LoadNextBlock() {
  while (true) {
    ++cur_block_idx_;
    if (cur_block_idx_ >= static_cast<int32_t>(candidate_blocks_.size())) {
      valid_ = false;
      return;
    }

    keys_buf_.clear();
    values_buf_.clear();
    shadowed_buf_.clear();

    Status s;
    // Same NewDataBlockIterator<DataBlockIter> call shape as
    // SABITableIterator::GetAllByIndexesFromDataBlock. Resetting biter_ unpins
    // the previously held block before opening the new one.
    // Readahead is RocksDB's own: the implicit ramp engages once reads
    // look sequential and resets when they do not.
    ReadOptions read_options;
    // Prefetched by the queue, or else RocksDB's implicit ramp.
    FilePrefetchBuffer* prefetch_buffer =
        prefetch_queue_.BufferFor(static_cast<size_t>(cur_block_idx_));
    if (prefetch_buffer == nullptr) {
      block_prefetcher_.PrefetchIfNeeded(
          bbt_->get_rep(), candidate_blocks_[cur_block_idx_],
          read_options.readahead_size,
          /*is_for_compaction=*/false, /*no_sequential_checking=*/false,
          read_options, /*readaheadsize_cb=*/nullptr,
          /*is_async_io_prefetch=*/false);
      prefetch_buffer = block_prefetcher_.prefetch_buffer();
    }
    DataBlockIter* new_biter = bbt_->NewDataBlockIterator<DataBlockIter>(
        read_options, candidate_blocks_[cur_block_idx_], nullptr,
        BlockType::kData, nullptr, nullptr, prefetch_buffer,
        false, false, s, true);
    biter_.reset(new_biter);
    if (!s.ok()) {
      // The candidate block is unreadable. Its rows cannot be skipped
      // silently, so stop the scan here and let the status propagate.
      status_ = s;
      valid_ = false;
      return;
    }

    // Scan EVERY entry in this candidate block; buffer exact matches.
    // prev_known is per block: the first entry's predecessor lives in the
    // previous block, which the candidate set may have pruned away, so it is
    // never observable here.
    bool prev_known = false;
    for (biter_->SeekToFirst(); biter_->Valid(); biter_->Next()) {
      ParsedInternalKey ikey;
      Status ps = rocksdb::ParseInternalKey(biter_->key(), &ikey, false);
      if (!ps.ok()) {
        prev_known = false;  // an unparsable entry breaks the adjacency chain
        continue;            // skip corrupted key
      }
      // Internal keys sort by user key ascending and seqno descending, so one
      // key's entries are contiguous with the newest first: a preceding entry
      // with the same user key means this row is an older in-file version.
      // Unknown counts as shadowed, which only costs a re-fetch. Unlike
      // SABITableIterator, which jumps to bitmap-selected rows and loses the
      // predecessor at every restart point, this walk steps on every entry, so
      // only a block's first row is unresolvable.
      bool in_file_shadowed = true;
      if (track_source_versions_) {
        in_file_shadowed =
            !prev_known || rocksdb::Slice(prev_user_key_) == ikey.user_key;
        prev_user_key_.assign(ikey.user_key.data(), ikey.user_key.size());
        prev_known = true;
      }
      // MVCC filtering.
      if (ikey.sequence > options_.read_seqno) continue;
      // Filter tombstones.
      if (ikey.type == rocksdb::kTypeDeletion ||
          ikey.type == rocksdb::kTypeSingleDeletion)
        continue;
      // Exact recheck of the full query (enforces OR clauses and any facts not
      // used for pruning).
      if (EmbeddedCodec::Evaluate(
              query_,
              std::string_view(biter_->value().data(), biter_->value().size()),
              options_)) {
        PinnableSlice k;
        k.PinSelf(biter_->key());  // key is delta-encoded -> copy.
        PinnableSlice v;
        v.PinSlice(biter_->value(), nullptr);  // value pinned (biter_ alive).
        keys_buf_.push_back(std::move(k));
        values_buf_.push_back(std::move(v));
        if (track_source_versions_)
          shadowed_buf_.push_back(in_file_shadowed ? 1 : 0);
      }
    }

    if (!keys_buf_.empty()) {
      buf_idx_ = 0;
      valid_ = true;
      return;
    }
    // No matches in this block; continue to the next candidate block.
  }
}

void EmbeddedTableIterator::GetReadaheadState(
    ReadaheadFileInfo* readahead_file_info) {
  if (block_prefetcher_.prefetch_buffer() != nullptr) {
    block_prefetcher_.prefetch_buffer()->GetReadaheadState(
        &(readahead_file_info->data_block_readahead_info));
  }
}

void EmbeddedTableIterator::SetReadaheadState(
    ReadaheadFileInfo* readahead_file_info) {
  // A zero readahead_size means no prior file built a prefetch buffer;
  // applying it would set this file's initial size to 0 and disable
  // auto-readahead entirely, so keep the fresh-start defaults instead.
  if (readahead_file_info->data_block_readahead_info.readahead_size > 0) {
    block_prefetcher_.SetReadaheadState(
        &(readahead_file_info->data_block_readahead_info));
  }
}

void EmbeddedTableIterator::SeekToFirst() {
  // A failure is sticky: never restart a scan that already lost data.
  if (!status_.ok()) {
    valid_ = false;
    return;
  }
  // The index block failed to load in the constructor: nothing to scan.
  if (idx_ == nullptr) {
    valid_ = false;
    return;
  }
  cur_block_idx_ = -1;
  buf_idx_ = 0;
  keys_buf_.clear();
  values_buf_.clear();
  valid_ = false;
  LoadNextBlock();
}

void EmbeddedTableIterator::Next() {
  assert(valid_);
  ++buf_idx_;
  if (buf_idx_ >= static_cast<int32_t>(keys_buf_.size())) {
    valid_ = false;
    LoadNextBlock();
  }
}

Slice EmbeddedTableIterator::key() const {
  assert(Valid());
  return keys_buf_[buf_idx_];
}

Slice EmbeddedTableIterator::value() const {
  assert(Valid());
  return values_buf_[buf_idx_];
}

}  // namespace experiment::embedded
