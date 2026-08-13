#pragma once
// Corresponds to the embedded baseline's EmbeddedDB (src/bindings/embedded/embedded_db.h);
// ported to namespace experiment::sai, installs SAIIndexFactory instead of
// EmbeddedIndexFactory, carries the SAI intersection_limit knob.
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#include <memory>
#include <string>
#include <variant>
#include <vector>
#include "bit_lsm_option.h"
#include "bit_lsm_query.h"
#include "sai_index_registry.h"

using Attr =
    std::variant<std::monostate, int64_t, uint64_t, double, std::string>;

namespace experiment::sai {

// The real SAIIterator is defined in sai_iterator.h. Only a forward
// declaration is needed here: SAIDB::NewIterator returns
// std::unique_ptr<SAIIterator>, but sai_db.cpp includes sai_iterator.h so the
// complete type is visible where NewIterator is defined and where unique_ptr's
// destructor is instantiated.
class SAIIterator;

class SAIDB {
 private:
  rocksdb::DB* db_;
  std::string db_path_;
  std::vector<rocksdb::ColumnFamilyHandle*> cf_handles_;
  rocksdb::Options rocksdb_options_;
  bit_lsm::BitLSMOptions bit_lsm_options_;
  int intersection_limit_;
  // On-demand index reads (Cassandra-like residency) instead of the resident
  // whole-blob reader. Off by default so existing experiments are unchanged
  // and the two modes can be compared as an A/B.
  bool ondemand_index_ = false;
  // Only populated in on-demand mode: per-SSTable directories, plus the
  // listener that retires them with their files.
  SAIIndexRegistry registry_;
  std::shared_ptr<rocksdb::Cache> block_cache_;

 public:
  SAIDB(const std::string& db_path, const bit_lsm::BitLSMOptions& bit_lsm_options,
        const rocksdb::Options& rocksdb_options,
        const rocksdb::BlockBasedTableOptions& table_options,
        int intersection_limit, bool ondemand_index = false);
  ~SAIDB();
  rocksdb::Status Put(const std::string& pk, const std::vector<Attr>& attrs,
                      const std::string& payload);
  std::unique_ptr<SAIIterator> NewIterator(bit_lsm::BitLSMQuery& query);
  rocksdb::DB* GetInternalDB() { return db_; }
  // Resident bytes the registry holds outside the block cache budget, so a run
  // can report the figure rather than assume it is negligible.
  size_t IndexRegistryMemoryUsage() const {
    return registry_.ApproximateMemoryUsage();
  }
  size_t IndexRegistrySize() const { return registry_.Size(); }
};

}  // namespace experiment::sai
