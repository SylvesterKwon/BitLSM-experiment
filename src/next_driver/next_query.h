#pragma once
// NEXT-isolated post-filter query representation + evaluator.
//
// Mirrors the semantics of honk::ParseFilters
// (src/honk_player/json_record_parser.h:160-250) and
// bit_lsm::BitLSMQuery::CheckCondition
// (third_party/BitLSM/src/include/bit_lsm_query.cpp:11-85), but is
// self-contained: nlohmann/json + nextd:: types only (next_schema.h).
//
// ISOLATION: do NOT include taxi_schema.h / json_record_parser.h / bit_lsm.h
// here (they pull in RocksDB 10.10.0 headers, breaking the NEXT-isolated
// build). This header is included by next_honk.cpp for the read/post-filter
// path only.

#include "json.hpp"
#include "next_schema.h"
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

namespace nextd {

// Reduced op set: this is exactly what honk::ParseFilters ever produces
// (eq -> EQUAL; in -> OR of EQUAL; range -> GREATER_EQUAL lo, LESS hi).
enum class CompareOp { EQUAL, GREATER_EQUAL, LESS };

struct Condition {
  int attr_idx;
  CompareOp op;
  Attr value;
};

// A clause is a group of conditions combined with OR. Multiple clauses are
// combined with AND (CNF), matching bit_lsm::BitLSMQuery's clause_groups.
using OrClause = std::vector<Condition>;

struct Query {
  std::vector<OrClause> clauses;
  std::vector<std::string> attr_names;  // distinct, encounter order
  // Task 3.3: optional "most_selective_attr" oracle hint (mirrors
  // honk::ParseFilters' hint_most_selective_attr, json_record_parser.h:144)
  // naming the attribute the PF driver should prefer to drive via NEXT.
  // -1 = no hint present.
  int hint_attr_idx = -1;
};

/// Extract a JSON scalar as Attr for a given attr type (mirrors honk's
/// ExtractValue: json_record_parser.h:148-158).
inline Attr ExtractValue(const nlohmann::json& jval, AttrType atype) {
  if (atype == AttrType::CATEGORICAL) {
    if (jval.is_string()) return jval.get<std::string>();
    if (jval.is_number_integer()) return std::to_string(jval.get<int64_t>());
    return std::to_string(jval.get<double>());
  }
  return jval.get<double>();
}

/// Mirrors honk::ParseFilters (json_record_parser.h:160-250): eq/in/range ->
/// clauses; collects distinct attr names in encounter order.
inline Query ParseFilters(const std::string& json_str) {
  Query q;
  std::unordered_set<std::string> seen_attrs;

  nlohmann::json doc = nlohmann::json::parse(json_str);
  for (const auto& f : doc.at("filters")) {
    std::string attr_name = f.at("attr").get<std::string>();
    int attr_idx = ColIndex(attr_name);
    if (attr_idx < 0)
      throw std::runtime_error("Unknown filter attr: " + attr_name);
    AttrType atype = Columns()[attr_idx].type;

    if (seen_attrs.insert(attr_name).second) q.attr_names.push_back(attr_name);

    std::string op_str = f.at("op").get<std::string>();
    if (op_str == "eq") {
      OrClause clause;
      clause.push_back(
          Condition{attr_idx, CompareOp::EQUAL, ExtractValue(f.at("value"), atype)});
      q.clauses.push_back(std::move(clause));
    } else if (op_str == "in") {
      OrClause clause;
      for (const auto& v : f.at("values"))
        clause.push_back(Condition{attr_idx, CompareOp::EQUAL, ExtractValue(v, atype)});
      q.clauses.push_back(std::move(clause));
    } else if (op_str == "range") {
      OrClause lo_clause;
      lo_clause.push_back(
          Condition{attr_idx, CompareOp::GREATER_EQUAL, ExtractValue(f.at("lo"), atype)});
      q.clauses.push_back(std::move(lo_clause));

      OrClause hi_clause;
      hi_clause.push_back(
          Condition{attr_idx, CompareOp::LESS, ExtractValue(f.at("hi"), atype)});
      q.clauses.push_back(std::move(hi_clause));
    } else {
      throw std::runtime_error("Unknown filter op: " + op_str);
    }
  }

  // Parse optional oracle hint: most_selective_attr (mirrors honk's
  // json_record_parser.h:238-246). Absent field or unknown attr name -> no
  // hint (hint_attr_idx stays -1); the PF driver falls back to the first
  // indexed attr with a usable predicate.
  if (doc.contains("most_selective_attr")) {
    std::string hint_name = doc.at("most_selective_attr").get<std::string>();
    int hint_idx = ColIndex(hint_name);
    if (hint_idx >= 0) q.hint_attr_idx = hint_idx;
  }

  return q;
}

/// Evaluate a single condition (mirrors BitLSMQuery::EvalCondition:
/// bit_lsm_query.cpp:11-45). Categorical: string equality/compare;
/// continuous: numeric compare.
inline bool EvalCondition(const Condition& cond, const Attr& attr_val) {
  if (std::holds_alternative<std::string>(cond.value)) {
    const std::string& target = std::get<std::string>(attr_val);
    const std::string& query_val = std::get<std::string>(cond.value);
    int cmp = target.compare(query_val);
    switch (cond.op) {
      case CompareOp::EQUAL: return cmp == 0;
      case CompareOp::GREATER_EQUAL: return cmp >= 0;
      case CompareOp::LESS: return cmp < 0;
    }
  } else {
    double val = std::get<double>(attr_val);
    double query_val = std::get<double>(cond.value);
    switch (cond.op) {
      case CompareOp::EQUAL: return val == query_val;
      case CompareOp::GREATER_EQUAL: return val >= query_val;
      case CompareOp::LESS: return val < query_val;
    }
  }
  return false;
}

/// Post-filter predicate evaluator: AND across clauses, OR within a clause.
/// `get_attr(attr_idx)` supplies the record's decoded value for that column
/// (see next_honk.cpp's read loop for the indexed/payload/sentinel routing).
inline bool Matches(const Query& q, const std::function<Attr(int)>& get_attr) {
  for (const auto& clause : q.clauses) {
    bool clause_pass = false;
    for (const auto& cond : clause) {
      if (EvalCondition(cond, get_attr(cond.attr_idx))) {
        clause_pass = true;
        break;  // OR short-circuit
      }
    }
    if (!clause_pass) return false;  // AND short-circuit
  }
  return true;
}

/// Collapse attr_idx's predicates into [lo,hi): >= lo sets lo; < hi sets hi;
/// eq v sets lo=hi=v (point). If attr_idx has no single-condition predicate
/// (e.g. no predicate at all, or an "in" OR-clause on attr_idx), lo/hi stay
/// at the full-scan defaults -DBL_MAX/+DBL_MAX -- the post-filter (Matches)
/// still yields correct results in that case, just without index pruning.
/// Returns whether attr_idx had at least one single-condition predicate.
inline bool RangeFor(const Query& q, int attr_idx, double& lo, double& hi) {
  lo = -std::numeric_limits<double>::max();
  hi = std::numeric_limits<double>::max();
  bool found = false;
  for (const auto& clause : q.clauses) {
    if (clause.size() != 1) continue;  // OR-group (e.g. "in") -> full scan
    const auto& cond = clause[0];
    if (cond.attr_idx != attr_idx) continue;
    double v = std::get<double>(cond.value);
    switch (cond.op) {
      case CompareOp::GREATER_EQUAL: lo = v; break;
      case CompareOp::LESS: hi = v; break;
      case CompareOp::EQUAL: lo = v; hi = v; break;
    }
    found = true;
  }
  return found;
}

// Task 3.3: an id guaranteed to never be a real CategoricalDict id (ids are
// assigned 0, 1, 2, ... in first-seen order -- see next_dict.h). Used to
// rewrite a query literal that the write pass never saw into a condition
// that provably matches nothing, rather than crashing or matching
// arbitrarily.
constexpr double kCategoricalNotFoundId = -1.0;

/// For every condition whose `attr_idx` is one of `indexed_cols` AND whose
/// value is still a string (i.e. a CATEGORICAL column), replace the string
/// literal with the double id `encode(attr_idx, literal, out_id)` produces
/// -- the SAME dictionary-encoded id the write path stored in that column's
/// indexed value slot (next_record.h). This makes an indexed CATEGORICAL
/// condition indistinguishable, from here on, from a CONTINUOUS one: both
/// RangeFor (above) and EvalCondition's numeric branch, and a post-filter
/// get_attr that just returns the raw indexed double, work unchanged.
///
/// `encode` returning false (literal never seen at write time) rewrites the
/// condition to `kCategoricalNotFoundId`, so it correctly matches nothing
/// rather than silently mis-comparing. Conditions on non-indexed or
/// CONTINUOUS columns (value already a double from ExtractValue) are left
/// untouched.
inline void EncodeIndexedCategoricals(
    Query& q, const std::vector<int>& indexed_cols,
    const std::function<bool(int /*attr_idx*/, const std::string& /*literal*/,
                              double& /*out_id*/)>& encode) {
  for (auto& clause : q.clauses) {
    for (auto& cond : clause) {
      if (!std::holds_alternative<std::string>(cond.value)) continue;
      bool is_indexed = false;
      for (int c : indexed_cols) {
        if (c == cond.attr_idx) {
          is_indexed = true;
          break;
        }
      }
      if (!is_indexed) continue;

      double id = kCategoricalNotFoundId;
      encode(cond.attr_idx, std::get<std::string>(cond.value), id);
      cond.value = id;
    }
  }
}

}  // namespace nextd
