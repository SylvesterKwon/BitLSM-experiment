#include "sai_plan.h"
#include <algorithm>
#include <numeric>

namespace experiment::sai {

namespace {
void TightenLower(SAIFact& f, double v, bool inc) {
  if (v > f.lo || (v == f.lo && f.lo_inc && !inc)) { f.lo = v; f.lo_inc = inc; }
}
void TightenUpper(SAIFact& f, double v, bool inc) {
  if (v < f.hi || (v == f.hi && f.hi_inc && !inc)) { f.hi = v; f.hi_inc = inc; }
}
}  // namespace

std::vector<SAIFact> ExtractFacts(const bit_lsm::BitLSMQuery& q,
                                  const bit_lsm::BitLSMOptions& opts) {
  std::vector<SAIFact> facts;
  auto find_cont = [&](uint32_t attr_idx) -> SAIFact* {
    for (auto& f : facts)
      if (!f.is_cat && f.attr_idx == attr_idx) return &f;
    return nullptr;
  };
  for (const auto& clause : q.clause_groups) {
    if (clause.size() != 1) continue;  // OR / non-single shapes: post-filter only
    const bit_lsm::QueryCondition& c = clause[0];
    if (c.attr_idx >= opts.attr_specs.size()) continue;
    if (opts.attr_specs[c.attr_idx].role == bit_lsm::AttrRole::UNORDERED) {
      if (c.op != bit_lsm::CompareOp::EQUAL) continue;
      SAIFact f;
      f.is_cat = true;
      f.attr_idx = c.attr_idx;
      f.cat_value = std::get<std::string>(c.value);
      facts.push_back(std::move(f));
    } else {
      const double v = std::get<double>(c.value);
      SAIFact* f = find_cont(c.attr_idx);
      if (f == nullptr) {
        SAIFact nf;
        nf.attr_idx = c.attr_idx;
        facts.push_back(std::move(nf));
        f = &facts.back();
      }
      switch (c.op) {
        case bit_lsm::CompareOp::EQUAL:
          TightenLower(*f, v, true);
          TightenUpper(*f, v, true);
          break;
        case bit_lsm::CompareOp::GREATER_EQUAL: TightenLower(*f, v, true);  break;
        case bit_lsm::CompareOp::GREATER:       TightenLower(*f, v, false); break;
        case bit_lsm::CompareOp::LESS_EQUAL:    TightenUpper(*f, v, true);  break;
        case bit_lsm::CompareOp::LESS:          TightenUpper(*f, v, false); break;
      }
    }
  }
  return facts;
}

std::vector<uint32_t> ChooseTopK(const std::vector<SAIFact>& facts, int limit) {
  std::vector<uint32_t> idx(facts.size());
  std::iota(idx.begin(), idx.end(), 0u);
  std::stable_sort(idx.begin(), idx.end(), [&](uint32_t a, uint32_t b) {
    return facts[a].est < facts[b].est;
  });
  if (limit > 0 && static_cast<size_t>(limit) < idx.size())
    idx.resize(static_cast<size_t>(limit));
  return idx;
}

}  // namespace experiment::sai
