#pragma once
// Corresponds to BitLSM's BitLSM wrapper (bit_lsm.h); independent, installs
// EmbeddedIndexFactory instead of SABIFactory.

#include <rocksdb/options.h>
#include <rocksdb/table.h>

#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "bit_lsm_option.h"
#include "bit_lsm_query.h"
#include "embedded_iterator.h"

using Attr =
    std::variant<std::monostate, int64_t, uint64_t, double, std::string>;

namespace experiment::embedded {

class EmbeddedDB {
 private:
  rocksdb::DB* db_;
  std::string db_path_;
  std::vector<rocksdb::ColumnFamilyHandle*> cf_handles_;
  rocksdb::Options rocksdb_options_;
  bit_lsm::BitLSMOptions bit_lsm_options_;
  uint32_t bloom_bits_;

 public:
  EmbeddedDB(const std::string& db_path,
             const bit_lsm::BitLSMOptions& bit_lsm_options,
             const rocksdb::Options& rocksdb_options,
             const rocksdb::BlockBasedTableOptions& table_options,
             uint32_t bloom_bits);
  ~EmbeddedDB();

  rocksdb::Status Put(const std::string& pk, const std::vector<Attr>& attrs,
                      const std::string& payload);

  std::unique_ptr<EmbeddedIterator> NewIterator(bit_lsm::BitLSMQuery& query);

  rocksdb::DB* GetInternalDB() { return db_; }
};

}  // namespace experiment::embedded
