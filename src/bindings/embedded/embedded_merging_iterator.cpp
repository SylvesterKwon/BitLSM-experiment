// Corresponds to BitLSM's BitLSMMergingIterator
// (third_party/BitLSM/src/include/bit_lsm_merging_iterator.cpp); ported,
// SABITableIterator replaced with EmbeddedTableIterator,
// BitLSMMemTableIterator replaced with EmbeddedMemTableIterator,
// BitLSMLevelIterator replaced with EmbeddedLevelIterator.

#include "embedded_iterator.h"

#include <bit_lsm_query.h>

#include <cassert>
#include <iostream>

#include "db/column_family.h"
#include "db/memtable.h"
#include "db/version_set.h"
#include "rocksdb/options.h"
#include "table/block_based/block_based_table_reader.h"
#include "table/format.h"

namespace experiment::embedded {

using namespace rocksdb;

EmbeddedMergingIterator::EmbeddedMergingIterator(SuperVersion* sv,
                                                 bit_lsm::BitLSMOptions options,
                                                 bit_lsm::BitLSMQuery query)
    : sv_(sv),
      cfd_(sv_->cfd),
      options_(options),
      query_(std::move(query)),
      v_(sv_->current),
      tc_(cfd_->table_cache()),
      storage_info_(v_->storage_info()),
      icmp_(storage_info_->InternalComparator()),
      cf_opts_(sv->mutable_cf_options),
      heap_(EmbeddedIteratorComparator(icmp_)) {
  // 1. Add MemTable iterators
  // 1-1. Active MemTable
  rocksdb::ReadOnlyMemTable* active_mem = sv_->mem;
  if (active_mem != nullptr) {
    rocksdb::MemTable* mem = static_cast<rocksdb::MemTable*>(active_mem);
    ch_iters_.push_back(new EmbeddedMemTableIterator(mem, options_, query_));
  }
  // 1-2. Immutable MemTables
  MemTableListVersion* memtable_list_version_ = sv_->imm;
  if (memtable_list_version_ != nullptr) {
    for (ReadOnlyMemTable* imm_mem : memtable_list_version_->GetMemTables()) {
      rocksdb::MemTable* mem = static_cast<rocksdb::MemTable*>(imm_mem);
      ch_iters_.push_back(new EmbeddedMemTableIterator(mem, options_, query_));
    }
  }

  // 2. Add L0 iterators (one EmbeddedTableIterator per L0 SST)
  TableCache::CacheInterface cache_interface = tc_->get_cache();
  const std::vector<FileMetaData*>& l0_files = storage_info_->LevelFiles(0);
  for (FileMetaData* meta : l0_files) {
    TableCache::TypedHandle* table_handle = nullptr;
    ReadOptions ro;
    Status s = tc_->FindTable(ro, FileOptions(), *icmp_, *meta, &table_handle,
                              cf_opts_);
    if (!s.ok()) {
      // Skipping the file would silently drop every row it holds, so record
      // the failure; SeekToFirst() refuses to scan once status_ is non-OK.
      std::cerr << "[EmbeddedMergingIterator]: Failed to load L0 file "
                << meta->fd.GetNumber() << ": " << s.ToString() << "\n";
      if (status_.ok()) status_ = s;
      continue;
    }
    TableReader* table = cache_interface.Value(table_handle);
    BlockBasedTable* bbt = static_cast<BlockBasedTable*>(table);

    ch_iters_.push_back(new EmbeddedTableIterator(bbt, options_, query_));
    l0_handles_.push_back(table_handle);
  }

  // 3. Add L1+ level iterators
  for (uint32_t level = 1; level < storage_info_->num_non_empty_levels();
       ++level) {
    if (storage_info_->NumLevelFiles(level) > 0) {
      ch_iters_.push_back(
          new EmbeddedLevelIterator(sv, level, options_, query_));
    }
  }
}

EmbeddedMergingIterator::~EmbeddedMergingIterator() {
  // 1. Free children iterators
  for (auto* ch : ch_iters_) delete ch;
  ch_iters_.clear();

  // 2. Release cache for L0 SSTs
  for (auto* handle : l0_handles_) tc_->get_cache().Release(handle);
  l0_handles_.clear();
}

void EmbeddedMergingIterator::SeekToFirst() {
  // 1. Clear existing heap
  while (!heap_.empty()) heap_.pop();
  valid_ = false;

  // 2. A child that failed to open (see the constructor) already lost rows:
  // never produce a partial merge over the survivors.
  if (!status_.ok()) return;

  // 3. Propagate SeekToFirst & register to heap
  for (auto* child : ch_iters_) {
    child->SeekToFirst();
    if (child->Valid()) {
      heap_.push(child);
      continue;
    }
    // An invalid child is end-of-data only while its status is OK. The first
    // non-OK status wins and ends the whole merge: the rows behind it are
    // unknown, so any output here would be an incomplete result.
    if (!child->status().ok()) {
      status_ = child->status();
      while (!heap_.empty()) heap_.pop();
      return;
    }
  }

  // 4. Set current iterator validity
  if (!heap_.empty()) {
    valid_ = true;
  }
}

void EmbeddedMergingIterator::Next() {
  assert(Valid());

  // 1. Forward top iterator in heap
  EmbeddedInternalIterator* top_iter = heap_.top();
  heap_.pop();
  top_iter->Next();
  if (top_iter->Valid()) {
    heap_.push(top_iter);
  } else if (!top_iter->status().ok()) {
    // The child stopped on an error rather than running out of rows; the
    // merge cannot complete, so stop instead of draining the other children.
    status_ = top_iter->status();
    while (!heap_.empty()) heap_.pop();
    valid_ = false;
    return;
  }

  // 2. Update valid_
  valid_ = !heap_.empty();
}

Slice EmbeddedMergingIterator::key() const {
  assert(Valid());
  return heap_.top()->key();
}

Slice EmbeddedMergingIterator::value() const {
  assert(Valid());
  return heap_.top()->value();
}

}  // namespace experiment::embedded
