#include "sai_index_registry.h"

#include <algorithm>
#include <iostream>

#include "file/filename.h"
#include "sai_blob_source.h"
#include "sai_coding.h"
#include "table/block_based/block_based_table_reader.h"

namespace experiment::sai {
using namespace rocksdb;

namespace {
constexpr uint32_t kMagic = 0x53414931;  // "SAI1", written by SAIIndexBuilder
// Footer: [region_off x n_attrs][u32 block_count][u32 n_attrs]
//         [u32 entries_total][u32 magic]
constexpr uint32_t kFooterFixedBytes = 16;
constexpr uint32_t kSectionARecordBytes = 12;  // psum, block offset, block size
}  // namespace

void SAIFileDirectory::Locate(uint32_t row, uint32_t& block_idx,
                              uint32_t& ordinal) const {
  auto it = std::upper_bound(entry_count_psum.begin(), entry_count_psum.end(), row);
  block_idx = static_cast<uint32_t>(it - entry_count_psum.begin());
  ordinal = row - (block_idx == 0 ? 0 : entry_count_psum[block_idx - 1]);
}

size_t SAIFileDirectory::ApproximateMemoryUsage() const {
  return sizeof(*this) + region_off.capacity() * sizeof(uint32_t) +
         entry_count_psum.capacity() * sizeof(uint32_t) +
         block_handles.capacity() *
             sizeof(UserDefinedIndexBuilder::BlockHandle);
}

bool BuildDirectoryFromSource(SAIBlobSource& src, SAIFileDirectory* out) {
  std::string buf;
  const uint32_t blob_size = static_cast<uint32_t>(src.BlobSize());
  if (blob_size < kFooterFixedBytes) return false;

  const char* footer =
      src.Read(blob_size - kFooterFixedBytes, kFooterFixedBytes, buf);
  if (!src.ok() || GetU32(footer + 12) != kMagic) return false;
  const uint32_t block_count = GetU32(footer);
  const uint32_t n_attrs = GetU32(footer + 4);
  out->entries_total = GetU32(footer + 8);

  // Region table sits just before the fixed footer.
  const uint32_t tail_off = blob_size - kFooterFixedBytes - 4 * n_attrs;
  const char* tail = src.Read(tail_off, 4 * n_attrs, buf);
  if (!src.ok()) return false;
  out->region_off.resize(n_attrs);
  for (uint32_t i = 0; i < n_attrs; ++i) out->region_off[i] = GetU32(tail + i * 4);

  // Section A in one read: it is contiguous at the head of the blob, and the
  // whole thing is needed to map a rowId to its data block.
  std::string section_a;
  const char* recs = src.Read(0, block_count * kSectionARecordBytes, section_a);
  if (!src.ok()) return false;
  out->entry_count_psum.resize(block_count);
  out->block_handles.resize(block_count);
  for (uint32_t i = 0; i < block_count; ++i) {
    const char* p = recs + i * kSectionARecordBytes;
    out->entry_count_psum[i] = GetU32(p);
    out->block_handles[i].offset = GetU32(p + 4);
    out->block_handles[i].size = GetU32(p + 8);
  }
  return true;
}

const SAIFileDirectory* SAIIndexRegistry::Get(uint64_t file_number,
                                              BlockBasedTable* bbt,
                                              std::shared_ptr<Cache> cache) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = map_.find(file_number);
    if (it != map_.end()) return it->second.get();
  }

  const BlockBasedTable::Rep* rep = bbt->get_rep();
  if (rep->udi_handle.IsNull() || rep->udi_handle.size() == 0) return nullptr;

  auto dir = std::make_unique<SAIFileDirectory>();
  dir->blob_offset = rep->udi_handle.offset();
  dir->blob_size = rep->udi_handle.size();

  SAIFileBlobSource src(rep->file.get(), dir->blob_offset, dir->blob_size,
                        file_number, cache);
  if (!BuildDirectoryFromSource(src, dir.get())) {
    std::cerr << "[SAIIndexRegistry] file " << file_number
              << ": could not parse the SAI blob directory\n";
    return nullptr;
  }

  std::lock_guard<std::mutex> lock(mu_);
  // Another thread may have built the same directory while this one read; keep
  // whichever landed first so callers never see two copies for one file.
  auto [it, inserted] = map_.emplace(file_number, std::move(dir));
  (void)inserted;
  return it->second.get();
}

void SAIIndexRegistry::Drop(uint64_t file_number) {
  std::lock_guard<std::mutex> lock(mu_);
  map_.erase(file_number);
}

size_t SAIIndexRegistry::ApproximateMemoryUsage() const {
  std::lock_guard<std::mutex> lock(mu_);
  size_t usage = sizeof(*this);
  for (const auto& [num, dir] : map_) {
    (void)num;
    usage += dir->ApproximateMemoryUsage();
  }
  return usage;
}

size_t SAIIndexRegistry::Size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return map_.size();
}

void SAIRegistryCleaner::OnTableFileDeleted(const TableFileDeletionInfo& info) {
  uint64_t number = 0;
  FileType type;
  // The listener reports a path, not a number; everything else about the file
  // is already gone by now.
  std::string name = info.file_path;
  const size_t slash = name.find_last_of('/');
  if (slash != std::string::npos) name = name.substr(slash + 1);
  if (ParseFileName(name, &number, &type) && type == kTableFile) {
    registry_->Drop(number);
  }
}

}  // namespace experiment::sai
