#pragma once
// Corresponds to the embedded baseline's EmbeddedDB (src/bindings/embedded/embedded_db.h);
// ported to namespace experiment::sai, installs SAIIndexFactory (Task 6) instead of
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

using Attr =
    std::variant<std::monostate, int64_t, uint64_t, double, std::string>;

namespace experiment::sai {

// Task 7 replaces this stub with the real intersection-based scan iterator
// (index-candidate resolution via MultiGet + per-clause predicate
// evaluation, mirroring EmbeddedIterator). It must be a complete type here,
// not just forward-declared: SAIDB::NewIterator returns
// std::unique_ptr<SAIIterator> by value, and GCC instantiates
// unique_ptr's destructor wherever such a function is defined -- even a
// function that only ever returns nullptr -- so an incomplete pointee type
// fails to compile (same rule as the Pimpl idiom).
class SAIIterator {
 public:
  void SeekToFirst() {}
  bool Valid() const { return false; }
  void Next() {}
};

class SAIDB {
 private:
  rocksdb::DB* db_;
  std::string db_path_;
  std::vector<rocksdb::ColumnFamilyHandle*> cf_handles_;
  rocksdb::Options rocksdb_options_;
  bit_lsm::BitLSMOptions bit_lsm_options_;
  int intersection_limit_;

 public:
  SAIDB(const std::string& db_path, const bit_lsm::BitLSMOptions& bit_lsm_options,
        const rocksdb::Options& rocksdb_options,
        const rocksdb::BlockBasedTableOptions& table_options,
        int intersection_limit);
  ~SAIDB();
  rocksdb::Status Put(const std::string& pk, const std::vector<Attr>& attrs,
                      const std::string& payload);
  std::unique_ptr<SAIIterator> NewIterator(bit_lsm::BitLSMQuery& query);
  rocksdb::DB* GetInternalDB() { return db_; }
};

}  // namespace experiment::sai
