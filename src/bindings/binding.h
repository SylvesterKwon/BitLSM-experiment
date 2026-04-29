#pragma once
#include "bit_lsm.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace experiment {

using ::Attr;  // global scope (bit_lsm.h)
using bit_lsm::AttrType;
using bit_lsm::BitLSMOptions;
using bit_lsm::BitLSMQuery;

struct ScanResult {
  uint64_t elapsed_ms;
  uint64_t matched;
};

struct WriteStats {
  uint64_t flush_bytes = 0;
  uint64_t compact_bytes = 0;
};

class Binding {
 public:
  virtual ~Binding() = default;

  virtual void Open(int argc, char* argv[], const std::string& db_path,
                    const BitLSMOptions& opts) = 0;

  virtual void Put(const std::string& pk, const std::vector<Attr>& attrs,
                   const std::string& payload) = 0;

  virtual ScanResult Scan(BitLSMQuery& query) = 0;

  virtual void Close() = 0;

  virtual std::string Name() const = 0;

  virtual std::string ParamSuffix() const { return ""; }

  // For WA experiment: flush memtable, wait for compactions to settle, and
  // return cumulative flush/compaction write bytes from RocksDB statistics.
  // Returns {0,0} when statistics is not enabled.
  virtual WriteStats GetWriteStats() { return {}; }
};

std::unique_ptr<Binding> CreateBinding(const std::string& name);

}  // namespace experiment
