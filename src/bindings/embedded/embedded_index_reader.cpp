#include "embedded_index.h"

#include <cstring>

#include "util/coding.h"  // RocksDB internal: DecodeFixed32/DecodeFixed64

namespace experiment::embedded {
using namespace rocksdb;

static double U2D(uint64_t u) { double d; std::memcpy(&d, &u, 8); return d; }

EmbeddedIndexReader::EmbeddedIndexReader(Slice& index_block,
                                         const bit_lsm::BitLSMOptions& opts,
                                         uint32_t bloom_bits_in,
                                         bool metadata_only)
    : options(opts), bloom_bits(bloom_bits_in), metadata_only_(metadata_only) {
  // Both modes parse the same small directory (Sections A/B/D + footer) into
  // owned vectors. Resident mode additionally copies the whole blob and
  // keeps base_ into that copy so BlockFilterRegion can serve Section C
  // (the bulky per-block payload) without a read; metadata mode parses
  // straight out of index_block and retains no pointer into it -- the block
  // is freed after construction (RetainsIndexContents() == false), so
  // keeping any would be a use-after-free.
  const char* base;
  if (metadata_only_) {
    base = index_block.data();
  } else {
    owned_.assign(index_block.data(), index_block.size());
    base_ = owned_.data();
    base = base_;
  }
  raw_len_ = index_block.size();
  const char* end = base + raw_len_;
  uint32_t B          = DecodeFixed32(end - 4 * sizeof(uint32_t));
  uint32_t sectionB_off = DecodeFixed32(end - 3 * sizeof(uint32_t));
  uint32_t sectionD_off = DecodeFixed32(end - 2 * sizeof(uint32_t));
  uint32_t num_cont   = DecodeFixed32(end - 1 * sizeof(uint32_t));

  entry_count_psum.resize(B);
  block_handles.resize(B);
  for (uint32_t i = 0; i < B; ++i) {
    const char* p = base + i * 3 * sizeof(uint32_t);
    entry_count_psum[i]    = DecodeFixed32(p);
    block_handles[i].offset = DecodeFixed32(p + sizeof(uint32_t));
    block_handles[i].size   = DecodeFixed32(p + 2 * sizeof(uint32_t));
  }
  section_c_off_.resize(B + 1);
  for (uint32_t i = 0; i <= B; ++i)
    section_c_off_[i] =
        DecodeFixed32(base + sectionB_off + i * sizeof(uint32_t));

  file_zone_.resize(num_cont);
  for (uint32_t i = 0; i < num_cont; ++i) {
    const char* p = base + sectionD_off + i * 2 * sizeof(uint64_t);
    file_zone_[i].min = U2D(DecodeFixed64(p));
    file_zone_[i].max = U2D(DecodeFixed64(p + sizeof(uint64_t)));
  }
}

UserDefinedIndexBuilder* EmbeddedIndexFactory::NewBuilder() const {
  return new EmbeddedIndexBuilder(options_, bloom_bits_);
}
std::unique_ptr<UserDefinedIndexReader> EmbeddedIndexFactory::NewReader(
    Slice& index_block) const {
  // Mode follows the factory flag so it can never disagree with
  // ProducesMetadataOnlyReaders(): the open path checks that BEFORE the
  // reader exists, to decide whether to cache the raw blob.
  return std::make_unique<EmbeddedIndexReader>(index_block, options_,
                                               bloom_bits_,
                                               options_.ondemand_index);
}

}  // namespace experiment::embedded
