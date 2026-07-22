// Assert-based unit test for sai_cont_index. Run: ./build/bin/sai_test_cont
#include "sai_cont_index.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace experiment::sai;

struct Pt { double v; uint32_t r; };

static std::string Build(const std::vector<Pt>& pts) {
  ContWriter w;
  for (const auto& p : pts) w.Add(p.v, p.r);
  std::string out;
  w.AppendTo(out);
  return out;
}

static std::vector<uint32_t> Oracle(const std::vector<Pt>& pts, double lo,
                                    bool loi, double hi, bool hii) {
  std::vector<uint32_t> got;
  for (const auto& p : pts) {
    bool ok_lo = loi ? (p.v >= lo) : (p.v > lo);
    bool ok_hi = hii ? (p.v <= hi) : (p.v < hi);
    if (ok_lo && ok_hi) got.push_back(p.r);
  }
  std::sort(got.begin(), got.end());
  return got;
}

static std::vector<uint32_t> Drain(RowCursor* c) {
  std::vector<uint32_t> got;
  if (c) while (c->Valid()) { got.push_back(c->Row()); c->Next(); }
  return got;
}

int main() {
  const double inf = std::numeric_limits<double>::infinity();
  // 1. Empty region.
  {
    std::string r = Build({});
    ContReader rd(r.data());
    assert(rd.Empty());
    assert(rd.Estimate(-inf, true, inf, true) == 0);
    assert(rd.OpenCursor(-inf, true, inf, true) == nullptr);
  }
  // 2. Small: fewer than one block, values interleaved with rowIds.
  {
    std::vector<Pt> pts = {{5.0, 10}, {1.0, 20}, {5.0, 3}, {2.5, 40}, {9.0, 1}};
    std::string r = Build(pts);
    ContReader rd(r.data());
    assert(!rd.Empty() && rd.MinValue() == 1.0 && rd.MaxValue() == 9.0);
    auto c = rd.OpenCursor(2.5, true, 5.0, true);
    assert(Drain(c.get()) == Oracle(pts, 2.5, true, 5.0, true));  // {3,10,40}
    auto c2 = rd.OpenCursor(2.5, false, 5.0, false);              // exclusive both
    assert(Drain(c2.get()) == Oracle(pts, 2.5, false, 5.0, false));
    auto c3 = rd.OpenCursor(100.0, true, 200.0, true);
    assert(Drain(c3.get()).empty());
    // Degenerate equality [v,v].
    auto c4 = rd.OpenCursor(5.0, true, 5.0, true);
    assert(Drain(c4.get()) == (std::vector<uint32_t>{3, 10}));
  }
  // 3. Multi-block (3.5 blocks) + estimate semantics (upper bound, exact when
  //    block-aligned) + fuzz vs oracle.
  {
    std::mt19937 gen(23);
    std::vector<Pt> pts;
    for (uint32_t i = 0; i < kContBlockEntries * 3 + 517; ++i)
      pts.push_back({static_cast<double>(gen() % 100000) / 10.0, i});
    std::string r = Build(pts);
    ContReader rd(r.data());
    // Full range: estimate exact, cursor returns every rowId.
    assert(rd.Estimate(-inf, true, inf, true) == pts.size());
    auto all = Drain(rd.OpenCursor(-inf, true, inf, true).get());
    assert(all == Oracle(pts, -inf, true, inf, true));
    for (int t = 0; t < 200; ++t) {
      double lo = static_cast<double>(gen() % 100000) / 10.0;
      double hi = lo + static_cast<double>(gen() % 20000) / 10.0;
      bool loi = gen() % 2, hii = gen() % 2;
      auto got = Drain(rd.OpenCursor(lo, loi, hi, hii).get());
      auto want = Oracle(pts, lo, loi, hi, hii);
      assert(got == want);
      assert(rd.Estimate(lo, loi, hi, hii) >= want.size());  // upper bound
    }
  }
  std::printf("sai_test_cont OK\n");
  return 0;
}
