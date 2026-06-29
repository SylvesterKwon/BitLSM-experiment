#pragma once
// Self-contained value layout + predicate evaluation for the Embedded baseline.
// Mirrors the FORMAT semantics of BitLSM's EncodeValue/DecodeAttr but is an
// independent implementation: it depends only on the harness API query/option
// types, never on BitLSM's bit_lsm_utils.h or BitLSMQuery::CheckCondition.
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "bit_lsm_option.h"  // bit_lsm::BitLSMOptions, AttrType  (harness API)
#include "bit_lsm_query.h"   // bit_lsm::BitLSMQuery, QueryCondition, CompareOp

using Attr = std::variant<double, std::string>;

namespace experiment::embedded {

// Layout: [u32 attr_cnt][u32 offset_0]...[u32 offset_{n-1}][u32 payload_offset]
//         [attr data...][payload...]
// Continuous attr = 8-byte double; categorical attr = bytes spanning
// [offset_i, offset_{i+1}) (offset_n == payload_offset).
class EmbeddedCodec {
 public:
  static void Encode(const bit_lsm::BitLSMOptions& opts,
                     const std::vector<Attr>& attrs, std::string_view payload,
                     std::string& out) {
    uint32_t n = static_cast<uint32_t>(attrs.size());
    uint32_t header = sizeof(uint32_t) * (1 + n + 1);
    uint32_t data = 0;
    for (uint32_t i = 0; i < n; ++i) {
      if (opts.attr_types[i] == bit_lsm::AttrType::CONTINUOUS)
        data += sizeof(double);
      else
        data += static_cast<uint32_t>(std::get<std::string>(attrs[i]).size());
    }
    out.resize(header + data + payload.size());
    char* base = out.data();
    std::memcpy(base, &n, sizeof(uint32_t));
    uint32_t off_pos = sizeof(uint32_t);
    char* dp = base + header;
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t cur = static_cast<uint32_t>(dp - base);
      std::memcpy(base + off_pos, &cur, sizeof(uint32_t));
      off_pos += sizeof(uint32_t);
      if (opts.attr_types[i] == bit_lsm::AttrType::CONTINUOUS) {
        double v = std::get<double>(attrs[i]);
        std::memcpy(dp, &v, sizeof(double));
        dp += sizeof(double);
      } else {
        const std::string& s = std::get<std::string>(attrs[i]);
        std::memcpy(dp, s.data(), s.size());
        dp += s.size();
      }
    }
    uint32_t poff = static_cast<uint32_t>(dp - base);
    std::memcpy(base + off_pos, &poff, sizeof(uint32_t));
    if (!payload.empty()) std::memcpy(dp, payload.data(), payload.size());
  }

  static std::variant<double, std::string_view> DecodeAttr(
      const bit_lsm::BitLSMOptions& opts, std::string_view value,
      uint32_t attr_idx) {
    const char* base = value.data();
    uint32_t off;
    std::memcpy(&off, base + sizeof(uint32_t) * (1 + attr_idx), sizeof(uint32_t));
    if (opts.attr_types[attr_idx] == bit_lsm::AttrType::CONTINUOUS) {
      double v;
      std::memcpy(&v, base + off, sizeof(double));
      return v;
    }
    uint32_t next;
    std::memcpy(&next, base + sizeof(uint32_t) * (1 + attr_idx + 1),
                sizeof(uint32_t));
    return std::string_view(base + off, next - off);
  }

  static bool Evaluate(const bit_lsm::BitLSMQuery& q, std::string_view value,
                       const bit_lsm::BitLSMOptions& opts) {
    for (const auto& clause : q.clause_groups) {
      bool clause_ok = false;  // OR within clause
      for (const auto& c : clause) {
        if (EvalOne(c, value, opts)) { clause_ok = true; break; }
      }
      if (!clause_ok) return false;  // AND across clauses
    }
    return true;
  }

 private:
  static bool EvalOne(const bit_lsm::QueryCondition& c, std::string_view value,
                      const bit_lsm::BitLSMOptions& opts) {
    auto a = DecodeAttr(opts, value, c.attr_idx);
    if (opts.attr_types[c.attr_idx] == bit_lsm::AttrType::CATEGORICAL) {
      return c.op == bit_lsm::CompareOp::EQUAL &&
             std::get<std::string_view>(a) == std::get<std::string>(c.value);
    }
    double x = std::get<double>(a);
    double v = std::get<double>(c.value);
    switch (c.op) {
      case bit_lsm::CompareOp::EQUAL:         return x == v;
      case bit_lsm::CompareOp::LESS_EQUAL:    return x <= v;
      case bit_lsm::CompareOp::GREATER_EQUAL: return x >= v;
      case bit_lsm::CompareOp::LESS:          return x < v;
      case bit_lsm::CompareOp::GREATER:       return x > v;
    }
    return false;
  }
};

}  // namespace experiment::embedded
