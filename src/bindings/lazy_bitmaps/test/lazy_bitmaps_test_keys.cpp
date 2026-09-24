// Assert-based unit test for lazy_bitmaps_keys.h. Run: ./build/bin/lazy_bitmaps_test_keys
#include "lazy_bitmaps_keys.h"

#include <cassert>
#include <cstdio>
#include <random>
#include <string>

using namespace experiment::lazy_bitmaps;

int main() {
  // Range keys order by (attr, bin), so one attribute's bins are contiguous.
  std::string a, b, c, d;
  RangeBinKey(1, 0, &a);
  RangeBinKey(1, 1, &b);
  RangeBinKey(1, 0xFFFF, &c);
  RangeBinKey(2, 0, &d);
  assert(a.size() == 8 && a < b && b < c && c < d);

  // Equality keys carry the attr prefix then the raw value.
  std::string e;
  EqualityKey(3, "abc", &e);
  assert(e.size() == 7 && e.substr(0, 4) == std::string("\0\0\0\3", 4) &&
         e.substr(4) == "abc");

  // Rowid keys roundtrip and order numerically.
  const uint64_t rowids[] = {0, 1, 255, 256, (1ull << 32), (1ull << 40) + 5,
                             UINT64_MAX};
  std::string prev;
  for (uint64_t r : rowids) {
    std::string k;
    RowidKey(r, &k);
    assert(k.size() == 8 && RowidFromKey(k) == r);
    assert(prev.empty() || prev < k);
    prev = k;
  }

  // The patched operand equals CRoaring's own serialization and decodes to
  // {rowid}.
  SingleRowidOperand operand;
  std::mt19937 gen(7);
  static const uint32_t fixed[] = {0u, 1u, 0xFFFFu, 0x10000u};
  for (int i = 0; i < 1000; ++i) {
    const uint32_t r = i < 4 ? fixed[i] : gen();
    roaring::Roaring want;
    want.add(r);
    std::string want_bytes(want.getSizeInBytes(true), '\0');
    want.write(want_bytes.data(), true);
    assert(operand.Bytes(r) == want_bytes);
    roaring::Roaring got = ReadBitmap(operand.Bytes(r));
    assert(got.cardinality() == 1 && got.contains(r));
  }

  // WriteBitmap/ReadBitmap roundtrip through the portable format.
  roaring::Roaring bm;
  for (uint32_t v = 0; v < 100000; v += 3) bm.add(v);
  std::string bytes;
  WriteBitmap(bm, &bytes);
  roaring::Roaring back = ReadBitmap(bytes);
  assert(back == bm);

  std::puts("lazy_bitmaps_test_keys: OK");
  return 0;
}
