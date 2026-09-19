#pragma once
// Row codec + predicate evaluation for the Embedded baseline.
//
// The row format is shared with every other method (bit_lsm's EncodeValue /
// DecodeAttr), and this class only forwards to it. It used to carry its own
// copy, which was byte-identical to BitLSM's format when the baseline landed
// and then drifted when BitLSM moved to the schema-derived v2 format: the old
// format spends four bytes on an offset per attribute where v2 packs
// fixed-width attributes into slots, so the same row came out 8 bytes larger at
// one attribute and 72 bytes larger at thirty-two. That is a difference in how
// a row is written, not in how it is indexed, and it landed on database size and
// on scan bytes -- the very things this baseline is compared on. Sharing the
// format removes it.
//
// What stays independent is what the baseline actually contributes: the
// per-block Bloom filter and zone map (embedded_index*), the block-pruning read
// stack, and the predicate evaluation below, which never calls
// BitLSMQuery::CheckCondition.
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "bit_lsm_option.h"  // bit_lsm::BitLSMOptions, IndexType  (harness API)
#include "bit_lsm_query.h"   // bit_lsm::BitLSMQuery, QueryCondition, CompareOp
#include "bit_lsm_utils.h"   // the shared row format: ValueLayout, Encode/Decode

namespace experiment::embedded {

class EmbeddedCodec {
 public:
  // The layout is derived from the schema once and reused: building one costs
  // four vector allocations, and these run per row.
  static void Encode(const bit_lsm::ValueLayout& layout,
                     const std::vector<Attr>& attrs, std::string_view payload,
                     std::string& out) {
    bit_lsm::EncodeValue(layout, attrs, payload, out);
  }

  static bit_lsm::AttrView DecodeAttr(const bit_lsm::ValueLayout& layout,
                                      std::string_view value,
                                      uint32_t attr_idx) {
    return bit_lsm::DecodeAttr(layout, value, attr_idx);
  }

  static bool Evaluate(const bit_lsm::BitLSMQuery& q, std::string_view value,
                       const bit_lsm::ValueLayout& layout) {
    for (const auto& clause : q.clause_groups) {
      bool clause_ok = false;  // OR within clause
      for (const auto& c : clause) {
        if (EvalOne(c, value, layout)) { clause_ok = true; break; }
      }
      if (!clause_ok) return false;  // AND across clauses
    }
    return true;
  }

 private:
  static bool EvalOne(const bit_lsm::QueryCondition& c, std::string_view value,
                      const bit_lsm::ValueLayout& layout) {
    auto a = DecodeAttr(layout, value, c.attr_idx);
    // A NULL attribute matches no comparison; the shared format can carry one
    // even though this experiment's schemas declare no nullable attribute.
    if (std::holds_alternative<std::monostate>(a)) return false;
    if (layout.specs[c.attr_idx].index_type == bit_lsm::IndexType::kEquality) {
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
