// Corresponds to the embedded baseline's EmbeddedDB (src/bindings/embedded/embedded_db.cpp);
// ported to namespace experiment::sai, installing SAIIndexFactory and wiring
// up SAIIterator.
#include "sai_db.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include "sai_index.h"
#include "sai_iterator.h"
#include "sai_value_codec.h"
#include "rocksdb/options.h"

using namespace std;
using namespace rocksdb;

namespace experiment::sai {

SAIDB::SAIDB(const string& db_path, const bit_lsm::BitLSMOptions& bit_lsm_options,
             const Options& rocksdb_options,
             const BlockBasedTableOptions& table_options, int intersection_limit,
             bool ondemand_index)
    : db_path_(db_path),
      bit_lsm_options_(bit_lsm_options),
      intersection_limit_(intersection_limit),
      ondemand_index_(ondemand_index) {
  // SAIIndexReader points into the cache entry's blob instead of copying it,
  // which is only sound while the block owns its bytes. Memory-mapped reads
  // are the one path where a block can reference file-mapped memory it does
  // not own (BlockFetcher::GetBlockContents), so refuse that combination
  // loudly rather than let it corrupt reads later.
  if (rocksdb_options.allow_mmap_reads) {
    throw std::runtime_error(
        "SAI does not support allow_mmap_reads: the index reader borrows the "
        "block cache entry's blob");
  }
  rocksdb_options_ = rocksdb_options;
  BlockBasedTableOptions opts = table_options;
  opts.user_defined_index_factory =
      make_shared<SAIIndexFactory>(bit_lsm_options_);
  if (ondemand_index_) {
    // RocksDB reads and parses the blob once when it opens a table, and with
    // cache_index_and_filter_blocks off it then pins that entry in the table
    // reader for the file's lifetime -- the whole index resident, defeating
    // the point of reading ranges on demand. Caching the entry instead lets it
    // age out, after which nothing re-fetches it: queries go through the
    // registry and the blob source.
    opts.cache_index_and_filter_blocks = true;
    // Registry directories and blob pages are keyed by file number, so a
    // reader that outlives its file would serve another file's bytes.
    rocksdb_options_.listeners.push_back(
        std::make_shared<SAIRegistryCleaner>(&registry_));
  }
  rocksdb_options_.table_factory.reset(NewBlockBasedTableFactory(opts));
  // Read back what the factory settled on: with no explicit block_cache it
  // creates one, and that is the cache the blob pages must share.
  block_cache_ = rocksdb_options_.table_factory->GetOptions<BlockBasedTableOptions>()
                     ->block_cache;

  ColumnFamilyOptions cf_opts(rocksdb_options_);
  cf_opts.level_compaction_dynamic_level_bytes = true;
  const vector<ColumnFamilyDescriptor> column_families(
      {ColumnFamilyDescriptor(kDefaultColumnFamilyName, cf_opts)});
  Status s =
      DB::Open(rocksdb_options_, db_path, column_families, &cf_handles_, &db_);
  if (!s.ok()) throw std::runtime_error("Failed to open DB: " + s.ToString());
}

SAIDB::~SAIDB() {
  for (auto handle : cf_handles_) db_->DestroyColumnFamilyHandle(handle);
  WaitForCompactOptions wait_for_compact_options = WaitForCompactOptions();
  wait_for_compact_options.close_db = true;
  Status s = db_->WaitForCompact(wait_for_compact_options);
  if (!s.ok()) cerr << "Failed to close DB: " << s.ToString() << "\n";
  delete db_;
  cout << "DB successfully closed\n";
}

Status SAIDB::Put(const string& pk, const vector<Attr>& attrs,
                  const string& payload) {
  if (attrs.size() != bit_lsm_options_.attr_num) {
    return Status::InvalidArgument(
        "The number of attrs does not match with db configuration.");
  }
  thread_local std::string serialized_value_buf;
  SAICodec::Encode(bit_lsm_options_, attrs, payload, serialized_value_buf);
  return db_->Put(WriteOptions(), pk, serialized_value_buf);
}

std::unique_ptr<SAIIterator> SAIDB::NewIterator(bit_lsm::BitLSMQuery& query) {
  if (!query.Validate(bit_lsm_options_).ok()) return nullptr;
  // Sort conditions within each OR clause by attr_idx (same as embedded_db.cpp:83-89).
  for (auto& clause : query.clause_groups) {
    std::sort(clause.begin(), clause.end(),
              [](const bit_lsm::QueryCondition& a,
                 const bit_lsm::QueryCondition& b) {
                return a.attr_idx < b.attr_idx;
              });
  }
  SAIIndexContext ctx;
  if (ondemand_index_) {
    ctx.registry = &registry_;
    ctx.cache = block_cache_;
  }
  return std::make_unique<SAIIterator>(db_, cf_handles_[0], bit_lsm_options_,
                                       query, intersection_limit_, ctx);
}

}  // namespace experiment::sai
