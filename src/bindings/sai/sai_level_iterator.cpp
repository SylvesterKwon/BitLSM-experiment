// Corresponds to the embedded baseline's EmbeddedLevelIterator
// (src/bindings/embedded/embedded_level_iterator.cpp); ported to namespace
// experiment::sai. SAI-specific changes are marked below.

#include "sai_iterator.h"

#include <bit_lsm_query.h>

#include <cassert>
#include <cstdint>
#include <iostream>

#include "db/column_family.h"
#include "db/version_set.h"
#include "rocksdb/options.h"
#include "table/block_based/block_based_table_reader.h"
#include "table/format.h"

namespace experiment::sai {

using namespace rocksdb;

SAILevelIterator::SAILevelIterator(SuperVersion* sv, uint32_t level,
                                   bit_lsm::BitLSMOptions options,
                                   bit_lsm::BitLSMQuery query, SAIPlan plan,
                                   bool track_source_versions)
    : sv_(sv),
      cfd_(sv->cfd),
      level_(level),
      options_(options),
      query_(std::move(query)),
      plan_(std::move(plan)),
      v_(sv->current),
      tc_(cfd_->table_cache()),
      storage_info_(v_->storage_info()),
      icmp_(storage_info_->InternalComparator()),
      cf_opts_(sv_->mutable_cf_options),
      files_(storage_info_->LevelFiles(level_)),
      cur_file_idx_(0),
      cur_table_handle_(nullptr),
      cur_sti_(nullptr),
      track_source_versions_(track_source_versions) {}

int SAILevelIterator::SourceLevel() const {
  return cur_sti_ ? cur_sti_->SourceLevel() : kMemtableSourceLevel;
}

uint64_t SAILevelIterator::SourceFileNumber() const {
  return cur_sti_ ? cur_sti_->SourceFileNumber() : 0;
}

bool SAILevelIterator::SourceHasNewerVersion() const {
  return cur_sti_ ? cur_sti_->SourceHasNewerVersion() : false;
}

SAILevelIterator::~SAILevelIterator() {
  if (cur_sti_) delete cur_sti_;
  if (cur_table_handle_) tc_->get_cache().Release(cur_table_handle_);
}

void SAILevelIterator::LoadFile(size_t idx) {
  // 1. Clean up existing iterator & table handle
  valid_ = false;
  TableCache::CacheInterface cache_interface = tc_->get_cache();
  if (cur_sti_ != nullptr) {
    // Capture the ramped readahead state before the iterator dies so the next
    // file resumes it instead of restarting the ramp from 8K.
    cur_sti_->GetReadaheadState(&readahead_file_info_);
    delete cur_sti_;
    cur_sti_ = nullptr;
  }
  if (cur_table_handle_ != nullptr) {
    cache_interface.Release(cur_table_handle_);
    cur_table_handle_ = nullptr;
  }

  // 2. Validate file index range
  if (idx >= files_.size()) {
    return;
  }

  // 3. Read BlockBasedTable
  const ReadOptions& read_options = ReadOptions();
  const FileOptions& file_options = *cfd_->soptions();
  TableCache::TypedHandle* new_table_handle = nullptr;
  const FileMetaData* file_meta = files_[idx];
  const bool no_io = false;

  Status s = tc_->FindTable(read_options, file_options, *icmp_, *file_meta,
                            &new_table_handle, cf_opts_, no_io);
  if (!s.ok()) {
    // Unreadable SST: its rows cannot be skipped silently, so record the
    // failure. cur_sti_ stays null and the callers below stop on status_.
    std::cerr << "Failed to load SST: " << s.ToString() << "\n";
    status_ = s;
    return;
  }
  TableReader* table = cache_interface.Value(new_table_handle);
  BlockBasedTable* bbt = static_cast<BlockBasedTable*>(table);

  // 4. Prepare new SAITableIterator
  cur_table_handle_ = new_table_handle;
  cur_sti_ = new SAITableIterator(
      bbt, options_, query_, plan_, track_source_versions_,
      static_cast<int>(level_), file_meta->fd.GetNumber(),
      file_meta->num_range_deletions > 0);
  cur_sti_->SetReadaheadState(&readahead_file_info_);
}

void SAILevelIterator::SeekToFirst() {
  // 0. A failure is sticky: never restart a scan that already lost a file.
  if (!status_.ok()) {
    valid_ = false;
    return;
  }

  // 1. Set file cursor to zero
  cur_file_idx_ = 0;

  // 2. Load files sequentially until valid data is found
  while (cur_file_idx_ < files_.size()) {
    LoadFile(cur_file_idx_);
    if (!status_.ok()) return;  // LoadFile failed; valid_ is already false

    if (cur_sti_ != nullptr) {
      cur_sti_->SeekToFirst();
      if (cur_sti_->Valid()) {
        valid_ = true;
        return;
      }
      // An invalid table iterator means "this file has no matching rows" only
      // while its status is OK. On an error, adopt it and stop: advancing
      // would drop every row of this file from the result.
      if (!cur_sti_->status().ok()) {
        status_ = cur_sti_->status();
        return;
      }
    }
    // If no valid data found, move next file
    cur_file_idx_++;
  }
  // If there's no valid data at all, valid_ is set to false
}

void SAILevelIterator::Next() {
  // 1. Check current validity
  assert(Valid());

  // 2. Advance current SAITableIterator
  cur_sti_->Next();

  // 3. If current table is no longer valid, move to next valid table
  if (!cur_sti_->Valid()) {
    valid_ = false;
    // Read the status before the next LoadFile() destroys cur_sti_: a
    // mid-file failure ends the scan here rather than skipping the rest.
    if (!cur_sti_->status().ok()) {
      status_ = cur_sti_->status();
      return;
    }
    while (true) {
      cur_file_idx_++;
      if (cur_file_idx_ >= files_.size()) {
        return;
      }
      LoadFile(cur_file_idx_);
      if (!status_.ok()) return;
      if (cur_sti_ != nullptr) {
        cur_sti_->SeekToFirst();
        if (cur_sti_->Valid()) {
          valid_ = true;
          return;
        }
        if (!cur_sti_->status().ok()) {
          status_ = cur_sti_->status();
          return;
        }
      }
    }
  }
}

Slice SAILevelIterator::key() const {
  assert(Valid());
  return cur_sti_->key();
}

Slice SAILevelIterator::value() const {
  assert(Valid());
  return cur_sti_->value();
}

}  // namespace experiment::sai
