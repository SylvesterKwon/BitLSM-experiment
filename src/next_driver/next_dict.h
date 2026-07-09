#pragma once
// NEXT-isolated categorical dictionary store (Phase 3 Task 3.3).
//
// A CATEGORICAL indexed attribute shares the same raw 8-byte double slot
// (value offset 8*p, see next_record.h) that CONTINUOUS indexed attributes
// use -- there is no room for a string there. This class maps each
// distinct string value of such an attribute to a stable double id,
// assigned in first-seen order during the WRITE pass:
//   - next_record.h::ParseRecord calls EncodeOrInsert() to get the id
//     stored in the indexed slot.
//   - next_query.h::EncodeIndexedCategoricals() calls Encode() (read-only:
//     no new ids) to translate a query literal (e.g. `payment_type == "1"`)
//     into the SAME id space, so RangeFor/EvalCondition/get_attr in
//     next_honk.cpp can treat the condition exactly like a CONTINUOUS one.
//
// The mapping must be IDENTICAL across process boundaries -- a write
// process assigns ids while producing the DB; a separate read process
// (Task 3.1/3.4's cross-process reopen) must decode/encode using the exact
// same mapping, or its point queries would target the wrong ids. Save()/
// Load() persist the mapping to a flat file next to the DB
// (`<db_path>/dict_<attr_name>.txt`, one "id\tvalue\n" per line).
//
// ISOLATION: no RocksDB / nlohmann dependency -- plain <fstream> only.

#include <cstdint>
#include <fstream>
#include <map>
#include <string>

namespace nextd {

class CategoricalDict {
 public:
  // Encode `value`, assigning the next sequential id (0, 1, 2, ...) if it
  // has not been seen before. Used on the WRITE path (next_record.h).
  double EncodeOrInsert(const std::string& value) {
    auto it = fwd_.find(value);
    if (it != fwd_.end()) return it->second;
    double id = static_cast<double>(fwd_.size());
    fwd_.emplace(value, id);
    return id;
  }

  // Encode `value` using the EXISTING mapping only (no insertion) -- used
  // to encode a QUERY literal on the READ path. Returns false if `value`
  // was never seen during the write pass, meaning the predicate cannot
  // match any record; `out_id` is left untouched in that case.
  bool Encode(const std::string& value, double& out_id) const {
    auto it = fwd_.find(value);
    if (it == fwd_.end()) return false;
    out_id = it->second;
    return true;
  }

  size_t size() const { return fwd_.size(); }

  // Persist the CURRENT mapping to `path` (overwrites any existing file).
  void Save(const std::string& path) const {
    std::ofstream out(path, std::ios::trunc);
    for (const auto& [value, id] : fwd_)
      out << static_cast<int64_t>(id) << '\t' << value << '\n';
  }

  // Load a mapping previously written by Save(), REPLACING the current
  // in-memory mapping. A missing file leaves the dict empty rather than
  // erroring -- callers on the read path only reach this when the write
  // pass indexed that categorical column, so an absent file indicates a
  // persistence gap the caller should surface (e.g. via a subsequent
  // Encode() miss), not a silent hard failure here.
  void Load(const std::string& path) {
    fwd_.clear();
    std::ifstream in(path);
    if (!in.is_open()) return;
    std::string line;
    while (std::getline(in, line)) {
      size_t tab = line.find('\t');
      if (tab == std::string::npos) continue;
      int64_t id = std::stoll(line.substr(0, tab));
      fwd_.emplace(line.substr(tab + 1), static_cast<double>(id));
    }
  }

 private:
  std::map<std::string, double> fwd_;
};

}  // namespace nextd
