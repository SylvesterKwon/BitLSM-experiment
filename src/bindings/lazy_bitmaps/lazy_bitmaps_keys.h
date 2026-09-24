#pragma once
// Key and operand encodings of the Lazy Bitmaps column families. Keys are
// fixed-width big-endian (the kEquality key carries the raw value after its
// attr prefix), so every bin of one attribute is one contiguous key span in
// lazy_bitmaps and rowid_map iterates in rowid order.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "roaring.hh"

namespace experiment::lazy_bitmaps {

inline void PutU32BE(uint32_t v, char* out) {
  out[0] = static_cast<char>(v >> 24);
  out[1] = static_cast<char>(v >> 16);
  out[2] = static_cast<char>(v >> 8);
  out[3] = static_cast<char>(v);
}

inline uint32_t GetU32BE(const char* p) {
  return (uint32_t{static_cast<uint8_t>(p[0])} << 24) |
         (uint32_t{static_cast<uint8_t>(p[1])} << 16) |
         (uint32_t{static_cast<uint8_t>(p[2])} << 8) |
         uint32_t{static_cast<uint8_t>(p[3])};
}

inline void PutU64BE(uint64_t v, char* out) {
  PutU32BE(static_cast<uint32_t>(v >> 32), out);
  PutU32BE(static_cast<uint32_t>(v), out + 4);
}

inline uint64_t GetU64BE(const char* p) {
  return (uint64_t{GetU32BE(p)} << 32) | GetU32BE(p + 4);
}

// lazy_bitmaps key of a kRange attribute: attr (u32 BE) | bin (u32 BE).
inline void RangeBinKey(uint32_t attr, uint32_t bin, std::string* out) {
  out->resize(8);
  PutU32BE(attr, out->data());
  PutU32BE(bin, out->data() + 4);
}

// lazy_bitmaps key of a kEquality attribute: attr (u32 BE) | raw value bytes.
// One bitmap per distinct value, exact.
inline void EqualityKey(uint32_t attr, std::string_view value,
                        std::string* out) {
  out->resize(4 + value.size());
  PutU32BE(attr, out->data());
  std::memcpy(out->data() + 4, value.data(), value.size());
}

// rowid_map key: rowid (u64 BE).
inline void RowidKey(uint64_t rowid, std::string* out) {
  out->resize(8);
  PutU64BE(rowid, out->data());
}

inline uint64_t RowidFromKey(std::string_view key) {
  return GetU64BE(key.data());
}

inline roaring::Roaring ReadBitmap(std::string_view bytes) {
  return roaring::Roaring::readSafe(bytes.data(), bytes.size());
}

// Portable serialization, run-optimized first as SABI does before it writes.
inline void WriteBitmap(roaring::Roaring& r, std::string* out) {
  r.runOptimize();
  out->resize(r.getSizeInBytes(true));
  r.write(out->data(), true);
}

// The merge operand of one put: the portable bytes of {rowid}. A one-element
// bitmap is a single array container, whose portable layout is fixed
// (cookie, container count, key/cardinality, offset, then the value), so the
// bytes come from patching a template serialized once and Put() allocates
// nothing. The constructor proves the patch against CRoaring itself.
class SingleRowidOperand {
 public:
  SingleRowidOperand() {
    roaring::Roaring zero;
    zero.add(0);
    buf_.resize(zero.getSizeInBytes(true));
    zero.write(buf_.data(), true);
    if (buf_.size() < kValOff + 2) Mismatch();
    for (uint32_t probe : {1u, 0x00010002u, 0x80000001u, 0xFFFFFFFFu}) {
      roaring::Roaring r;
      r.add(probe);
      std::string want(r.getSizeInBytes(true), '\0');
      r.write(want.data(), true);
      if (Bytes(probe) != want) Mismatch();
    }
  }

  // Valid until the next call on this object.
  std::string_view Bytes(uint32_t rowid) {
    // Container key = high 16 bits, value = low 16 bits, both u16 LE.
    buf_[kKeyOff] = static_cast<char>(rowid >> 16);
    buf_[kKeyOff + 1] = static_cast<char>(rowid >> 24);
    buf_[kValOff] = static_cast<char>(rowid);
    buf_[kValOff + 1] = static_cast<char>(rowid >> 8);
    return buf_;
  }

 private:
  static constexpr size_t kKeyOff = 8;   // after cookie (4) + size (4)
  static constexpr size_t kValOff = 16;  // after key/card (4) + offset (4)
  std::string buf_;

  [[noreturn]] static void Mismatch() {
    std::fprintf(stderr,
                 "[LazyBitmaps] one-element operand template does not match "
                 "CRoaring's portable layout\n");
    std::abort();
  }
};

}  // namespace experiment::lazy_bitmaps
