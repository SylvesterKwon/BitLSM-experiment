#pragma once
// Conjunction planning for the SAI baseline.
// ExtractFacts: single-condition flat-AND clauses only (fairness rule, design §7);
// same fact-building shape as the embedded baseline's SelectCandidateBlocks
// (src/bindings/embedded/embedded_table_iterator.cpp:55-136), with range
// tightening on repeated bounds.
// ChooseTopK corresponds to Cassandra's KeyRangeIntersectionIterator.Builder
// .buildIterator (iterators/KeyRangeIntersectionIterator.java:290-315): sort
// ascending by estimated count, keep the `limit` most selective; limit<=0 or
// limit>=n keeps all. Default limit 2 = cassandra.sai.intersection_clause_limit
// (config/CassandraRelevantProperties.java:489).
#include <cstdint>
#include <limits>
#include <string>
#include <vector>
#include "bit_lsm_option.h"
#include "bit_lsm_query.h"

namespace experiment::sai {

struct SAIFact {
  bool is_cat = false;
  uint32_t attr_idx = 0;
  std::string cat_value;
  double lo = -std::numeric_limits<double>::infinity();
  double hi = std::numeric_limits<double>::infinity();
  bool lo_inc = true, hi_inc = true;
  uint64_t est = 0;  // filled by the ranking pass (Task 8)
};

struct SAIPlan {
  std::vector<SAIFact> chosen;  // facts used for index intersection (<= limit)
  bool full_scan = false;       // no usable facts -> embedded-style scan of all blocks
};

std::vector<SAIFact> ExtractFacts(const bit_lsm::BitLSMQuery& q,
                                  const bit_lsm::BitLSMOptions& opts);
std::vector<uint32_t> ChooseTopK(const std::vector<SAIFact>& facts, int limit);

}  // namespace experiment::sai
