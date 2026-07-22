// Assert-based unit test for sai_postings. Run: ./build/bin/sai_test_postings
#include "sai_postings.h"
#include <cassert>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace experiment::sai;

static std::string Ser(const std::vector<uint32_t>& rows) {
  PostingsBuilder b;
  for (uint32_t r : rows) b.Add(r);
  std::string out;
  b.AppendTo(out);
  return out;
}

static std::vector<uint32_t> Drain(RowCursor& c) {
  std::vector<uint32_t> got;
  while (c.Valid()) { got.push_back(c.Row()); c.Next(); }
  return got;
}

int main() {
  // 1. Round-trip: empty, single, exactly 128, 128+1, large random.
  {
    std::string s = Ser({});
    PostingsCursor c(s.data());
    assert(c.Count() == 0 && !c.Valid());
  }
  for (uint32_t n : {1u, 127u, 128u, 129u, 1024u}) {
    std::vector<uint32_t> rows;
    for (uint32_t i = 0; i < n; ++i) rows.push_back(i * 7 + 3);
    std::string s = Ser(rows);
    PostingsCursor c(s.data());
    assert(c.Count() == n);
    assert(Drain(c) == rows);
  }
  {
    std::mt19937 gen(7);
    std::vector<uint32_t> rows;
    uint32_t cur = 0;
    for (int i = 0; i < 50000; ++i) { cur += 1 + gen() % 1000; rows.push_back(cur); }
    std::string s = Ser(rows);
    PostingsCursor c(s.data());
    assert(Drain(c) == rows);
  }
  // 2. AdvanceTo: exact hit, between values, before first, past last, repeated.
  {
    std::string s = Ser({10, 20, 30, 300, 4000});
    PostingsCursor c(s.data());
    c.AdvanceTo(20); assert(c.Valid() && c.Row() == 20);
    c.AdvanceTo(21); assert(c.Valid() && c.Row() == 30);
    c.AdvanceTo(30); assert(c.Valid() && c.Row() == 30);  // no-op when already there
    c.AdvanceTo(301); assert(c.Valid() && c.Row() == 4000);
    c.AdvanceTo(4001); assert(!c.Valid());
  }
  {  // AdvanceTo across skip-table blocks
    std::vector<uint32_t> rows;
    for (uint32_t i = 0; i < 1000; ++i) rows.push_back(i * 10);
    std::string s = Ser(rows);
    PostingsCursor c(s.data());
    c.AdvanceTo(5005); assert(c.Valid() && c.Row() == 5010);
    c.AdvanceTo(9990); assert(c.Valid() && c.Row() == 9990);
  }
  // 3. IntersectionCursor: 2-way and 3-way, disjoint, subset, empty child.
  {
    auto mk = [](std::vector<uint32_t> v) {
      auto s = std::make_shared<std::string>(Ser(v));
      struct Owning : PostingsCursor {
        std::shared_ptr<std::string> keep;
        Owning(std::shared_ptr<std::string> s) : PostingsCursor(s->data()), keep(s) {}
      };
      return std::unique_ptr<RowCursor>(new Owning(s));
    };
    std::vector<std::unique_ptr<RowCursor>> cs;
    cs.push_back(mk({1, 3, 5, 7, 9, 100}));
    cs.push_back(mk({2, 3, 4, 7, 100}));
    IntersectionCursor ic(std::move(cs));
    assert(Drain(ic) == (std::vector<uint32_t>{3, 7, 100}));

    std::vector<std::unique_ptr<RowCursor>> cs2;
    cs2.push_back(mk({1, 2, 3}));
    cs2.push_back(mk({4, 5, 6}));
    IntersectionCursor ic2(std::move(cs2));
    assert(Drain(ic2).empty());

    std::vector<std::unique_ptr<RowCursor>> cs3;
    cs3.push_back(mk({5, 10, 15, 20}));
    cs3.push_back(mk({10, 20}));
    cs3.push_back(mk({0, 10, 20, 30}));
    IntersectionCursor ic3(std::move(cs3));
    assert(Drain(ic3) == (std::vector<uint32_t>{10, 20}));
  }
  // 4. 32-bit delta round-trip: exercises BitsFor at bpv==32 (a delta of
  // exactly 2^31), the path touched by the fix for a prior UB bug in the
  // naive `while ((v >> b) != 0) ++b;` shift-by-type-width loop.
  {
    std::vector<uint32_t> rows = {0u, 0x80000000u};
    std::string s = Ser(rows);
    PostingsCursor c(s.data());
    assert(Drain(c) == rows);
    assert(!c.Valid());
  }
  {  // mix of a large (>=2^31) delta with small deltas in the same block.
    std::vector<uint32_t> rows = {5u, 7u, 0x80000005u, 0x80000007u};
    std::string s = Ser(rows);
    PostingsCursor c(s.data());
    assert(Drain(c) == rows);
  }
  // 5. IntersectionCursor::AdvanceTo: skip ahead across children, no-op when
  // the target is behind the current row, and past-end invalidation.
  {
    auto mk = [](std::vector<uint32_t> v) {
      auto s = std::make_shared<std::string>(Ser(v));
      struct Owning : PostingsCursor {
        std::shared_ptr<std::string> keep;
        Owning(std::shared_ptr<std::string> s) : PostingsCursor(s->data()), keep(s) {}
      };
      return std::unique_ptr<RowCursor>(new Owning(s));
    };
    std::vector<std::unique_ptr<RowCursor>> cs;
    cs.push_back(mk({10, 20, 50, 60, 90, 95}));
    cs.push_back(mk({5, 10, 15, 50, 55, 90}));
    cs.push_back(mk({10, 30, 50, 70, 90, 100}));
    IntersectionCursor ic(std::move(cs));  // common rows: {10, 50, 90}
    ic.AdvanceTo(50);
    assert(ic.Valid() && ic.Row() == 50);
    ic.AdvanceTo(0);  // below current row -> no-op
    assert(ic.Valid() && ic.Row() == 50);
    ic.AdvanceTo(51);
    assert(ic.Valid() && ic.Row() == 90);
    ic.AdvanceTo(1000);
    assert(!ic.Valid());
  }
  // 6. IntersectionCursor with an empty-list child: intersection is empty
  // from the very start, regardless of the other children's contents.
  {
    auto mk = [](std::vector<uint32_t> v) {
      auto s = std::make_shared<std::string>(Ser(v));
      struct Owning : PostingsCursor {
        std::shared_ptr<std::string> keep;
        Owning(std::shared_ptr<std::string> s) : PostingsCursor(s->data()), keep(s) {}
      };
      return std::unique_ptr<RowCursor>(new Owning(s));
    };
    std::vector<std::unique_ptr<RowCursor>> cs;
    cs.push_back(mk({}));
    cs.push_back(mk({1, 2, 3}));
    cs.push_back(mk({1, 2, 3}));
    IntersectionCursor ic(std::move(cs));
    assert(!ic.Valid());
    assert(Drain(ic).empty());
  }
  std::printf("sai_test_postings OK\n");
  return 0;
}
