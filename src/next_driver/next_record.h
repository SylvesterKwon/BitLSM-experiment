#pragma once
// Record parser + NEXT value codec.
//
// Mirrors the field-routing semantics of honk::RecordParser::ParseRecord
// (src/honk_player/json_record_parser.h:46-137), but:
//  (a) routes N indexed columns to raw `double`s (not Attrs) -- one per
//      co-resident 1D sec-index (Phase 2 Task 2.2-2.4: the engine builds N
//      per-SST sec-index blocks + N global R-trees; index j reads the
//      double at value offset 8*j), and
//  (b) routes ALL OTHER columns into a length-prefixed payload blob, so
//      any predicate can be post-filtered later (Group C).
//
// ISOLATION: uses nextd:: types (next_schema.h) + nlohmann/json exclusively.
// Do NOT include taxi_schema.h / json_record_parser.h here (they pull in
// bit_lsm.h -> RocksDB 10.10.0 headers, breaking the NEXT-isolated build).

#include "json.hpp"
#include "next_schema.h"
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace nextd {

/// Append a length-prefixed field to buf: [4B col_idx][4B len][len bytes]
inline void AppendPayloadField(std::string& buf, uint32_t col_idx,
                                const void* data, uint32_t len) {
  buf.append(reinterpret_cast<const char*>(&col_idx), 4);
  buf.append(reinterpret_cast<const char*>(&len), 4);
  buf.append(reinterpret_cast<const char*>(data), len);
}

/// Parse a JSON taxi record.
///
/// `indexed_cols[p]` is the column index routed to `out_indexed[p]` (raw
/// double; default -1.0 if null/absent). An entry may name a CONTINUOUS
/// column (the JSON double is used directly) OR a CATEGORICAL column (Task
/// 3.3: the string is dictionary-encoded to a stable double id via
/// `encode_categorical(col_idx, value)` -- see next_dict.h::CategoricalDict;
/// callers indexing a categorical column MUST supply it). `out_indexed` is
/// resized to `indexed_cols.size()`. Every OTHER recognized column is packed
/// into `out_payload` as [4B col_idx][4B len][bytes]: CONTINUOUS -> 8B
/// little-endian double; CATEGORICAL -> UTF-8 string bytes (numeric JSON
/// values are stringified, matching honk::RecordParser); null -> sentinel
/// (-1.0 continuous / "Null" categorical). Unrecognized JSON keys are
/// ignored.
inline void ParseRecord(
    const std::string& json_str, const std::vector<int>& indexed_cols,
    std::vector<double>& out_indexed, std::string& out_payload,
    const std::function<double(int /*col_idx*/, const std::string& /*value*/)>&
        encode_categorical = {}) {
  const auto& cols = Columns();
  out_indexed.assign(indexed_cols.size(), -1.0);
  out_payload.clear();

  nlohmann::json doc = nlohmann::json::parse(json_str);

  for (auto it = doc.begin(); it != doc.end(); ++it) {
    int col_idx = ColIndex(it.key());
    if (col_idx < 0) continue;  // unrecognized field

    const nlohmann::json& jval = it.value();
    AttrType atype = cols[col_idx].type;
    bool is_null = jval.is_null();

    std::string str_val;
    double dbl_val = -1.0;
    if (is_null) {
      if (atype == AttrType::CATEGORICAL) str_val = "Null";
      // else: dbl_val stays at the -1.0 sentinel
    } else if (atype == AttrType::CATEGORICAL) {
      if (jval.is_string()) {
        str_val = jval.get<std::string>();
      } else if (jval.is_number_integer()) {
        str_val = std::to_string(jval.get<int64_t>());
      } else {
        str_val = std::to_string(jval.get<double>());
      }
    } else {
      dbl_val = jval.get<double>();
    }

    // Small N (Phase 2: 1-2 co-resident indexes) -- a linear scan to find
    // col_idx's position among indexed_cols is cheap and keeps this a plain
    // vector (matches offset 8*p in the emitted value, p = list position).
    int pos = -1;
    for (size_t p = 0; p < indexed_cols.size(); p++) {
      if (indexed_cols[p] == col_idx) {
        pos = static_cast<int>(p);
        break;
      }
    }

    if (pos >= 0) {
      if (atype == AttrType::CATEGORICAL) {
        // Task 3.3: dictionary-encode the string into the same 8-byte slot
        // a CONTINUOUS indexed column would use (str_val is already "Null"
        // for a null value, so it round-trips through the dict like any
        // other distinct string).
        out_indexed[pos] = encode_categorical(col_idx, str_val);
      } else {
        out_indexed[pos] = dbl_val;
      }
    } else if (atype == AttrType::CATEGORICAL) {
      AppendPayloadField(out_payload, static_cast<uint32_t>(col_idx),
                         str_val.data(), static_cast<uint32_t>(str_val.size()));
    } else {
      AppendPayloadField(out_payload, static_cast<uint32_t>(col_idx),
                         &dbl_val, sizeof(double));
    }
  }
}

/// Given a NEXT value ([8B*num_indexed doubles][payload]), find `col_idx` in
/// the payload and decode it to Attr. Returns false if absent. `num_indexed`
/// is the count of leading 8B indexed doubles to skip (N).
inline bool DecodePayloadAttr(const std::string& value, int col_idx,
                              uint32_t num_indexed, Attr& out) {
  size_t indexed_bytes = 8 * static_cast<size_t>(num_indexed);
  if (value.size() < indexed_bytes) return false;
  const auto& cols = Columns();
  size_t pos = indexed_bytes;  // skip the leading indexed doubles
  while (pos + 8 <= value.size()) {
    uint32_t idx = 0, len = 0;
    std::memcpy(&idx, value.data() + pos, 4);
    std::memcpy(&len, value.data() + pos + 4, 4);
    pos += 8;
    if (pos + len > value.size()) break;  // malformed guard

    if (static_cast<int>(idx) == col_idx) {
      if (idx < cols.size() && cols[idx].type == AttrType::CATEGORICAL) {
        out = std::string(value.data() + pos, len);
      } else {
        double d = 0.0;
        if (len == sizeof(double))
          std::memcpy(&d, value.data() + pos, sizeof(double));
        out = d;
      }
      return true;
    }
    pos += len;
  }
  return false;
}

/// value = [8B double indexed_0][8B double indexed_1]...[payload]. Reads the
/// double at list-position `p` (default 0), matching the engine's index p
/// reading the double at value offset 8*p.
inline double IndexedValue(const std::string& value, uint32_t p = 0) {
  return *reinterpret_cast<const double*>(value.data() + 8 * static_cast<size_t>(p));
}

}  // namespace nextd
