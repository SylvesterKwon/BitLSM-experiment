// Assert-based unit test for sai_plan. Run: ./build/bin/sai_test_plan
#include "sai_plan.h"
#include <cassert>
#include <cstdio>
#include <limits>

using namespace experiment::sai;
using bit_lsm::IndexType;
using bit_lsm::AttrSpec;
using bit_lsm::BitLSMOptions;
using bit_lsm::BitLSMQuery;
using bit_lsm::CompareOp;
using bit_lsm::QueryCondition;

int main() {
  const double inf = std::numeric_limits<double>::infinity();
  BitLSMOptions opts;
  opts.attr_num = 3;
  opts.attr_specs = {AttrSpec(bit_lsm::IndexType::kEquality, bit_lsm::PhysicalType::kVarBinary, 0), AttrSpec(bit_lsm::IndexType::kRange, bit_lsm::PhysicalType::kFloat, 8),
                     AttrSpec(bit_lsm::IndexType::kRange, bit_lsm::PhysicalType::kFloat, 8)};

  // 1. cat eq + two cont conds on the same attr coalesce (tightened), OR skipped.
  {
    BitLSMQuery q(std::vector<bit_lsm::OrClause>{
        {{0, CompareOp::EQUAL, std::string("red")}},
        {{1, CompareOp::GREATER, 5.0}},
        {{1, CompareOp::LESS_EQUAL, 10.0}},
        {{2, CompareOp::EQUAL, 7.0}},
        {{0, CompareOp::EQUAL, std::string("a")},
         {0, CompareOp::EQUAL, std::string("b")}},  // OR clause -> skipped
    });
    auto facts = ExtractFacts(q, opts);
    assert(facts.size() == 3);
    assert(facts[0].is_cat && facts[0].attr_idx == 0 && facts[0].cat_value == "red");
    assert(!facts[1].is_cat && facts[1].attr_idx == 1);
    assert(facts[1].lo == 5.0 && !facts[1].lo_inc && facts[1].hi == 10.0 && facts[1].hi_inc);
    assert(facts[2].lo == 7.0 && facts[2].lo_inc && facts[2].hi == 7.0 && facts[2].hi_inc);
  }
  // 2. Tightening keeps the stricter bound regardless of clause order.
  {
    BitLSMQuery q(std::vector<bit_lsm::OrClause>{
        {{1, CompareOp::GREATER, 7.0}}, {{1, CompareOp::GREATER, 5.0}}});
    auto facts = ExtractFacts(q, opts);
    assert(facts.size() == 1 && facts[0].lo == 7.0 && !facts[0].lo_inc &&
           facts[0].hi == inf);
  }
  // 3. Only-OR query -> no facts.
  {
    BitLSMQuery q(std::vector<bit_lsm::OrClause>{
        {{0, CompareOp::EQUAL, std::string("a")},
         {0, CompareOp::EQUAL, std::string("b")}}});
    assert(ExtractFacts(q, opts).empty());
  }
  // 4. ChooseTopK: sort asc by est, stable on ties; limit<=0 or >=n keeps all.
  {
    std::vector<SAIFact> f(4);
    f[0].est = 50; f[1].est = 10; f[2].est = 50; f[3].est = 5;
    auto k2 = ChooseTopK(f, 2);
    assert((k2 == std::vector<uint32_t>{3, 1}));
    auto k0 = ChooseTopK(f, 0);
    assert((k0 == std::vector<uint32_t>{3, 1, 0, 2}));  // stable: 0 before 2
    auto k9 = ChooseTopK(f, 9);
    assert(k9.size() == 4);
    auto k1 = ChooseTopK(f, 1);
    assert((k1 == std::vector<uint32_t>{3}));
  }
  std::printf("sai_test_plan OK\n");
  return 0;
}
