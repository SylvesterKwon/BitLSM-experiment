#pragma once
// Self-contained Bloom filter for the Embedded baseline (per block, per
// categorical attribute). Standard double-hashing; k derived from bits-per-key.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace experiment::embedded {

inline uint64_t BloomHash(std::string_view s) {
  // FNV-1a 64-bit.
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
  return h;
}

inline uint32_t BloomNumHashes(uint32_t bits_per_key) {
  int k = static_cast<int>(std::lround(bits_per_key * 0.6931471805599453));
  return static_cast<uint32_t>(std::clamp(k, 1, 30));
}

class BloomBuilder {
 public:
  void Add(std::string_view key) { hashes_.push_back(BloomHash(key)); }

  std::string Finish(uint32_t bits_per_key) const {
    uint32_t nbits = std::max<uint32_t>(
        1, static_cast<uint32_t>(hashes_.size()) * bits_per_key);
    // Round up to a multiple of 8 so that stored_bytes*8 == nbits used to
    // place bits. Without this, callers reconstructing nbits as bits.size()*8
    // would get a larger nbits, causing bit positions to map differently and
    // producing false negatives.
    nbits = ((nbits + 7) / 8) * 8;
    uint32_t nbytes = nbits / 8;
    std::string out(nbytes, '\0');
    uint32_t k = BloomNumHashes(bits_per_key);
    for (uint64_t h : hashes_) {
      uint32_t h1 = static_cast<uint32_t>(h);
      uint32_t h2 = static_cast<uint32_t>(h >> 32);
      for (uint32_t j = 0; j < k; ++j) {
        uint32_t bit = (h1 + j * h2) % nbits;
        out[bit >> 3] |= static_cast<unsigned char>(1u << (bit & 7));
      }
    }
    return out;
  }

 private:
  std::vector<uint64_t> hashes_;
};

// Query the Bloom filter stored in `bits` (length stored_bytes).
// Caller contract: pass nbits = stored_bytes*8, which by construction equals
// the nbits used at build time (BloomBuilder::Finish rounds up to a multiple
// of 8 before placing bits, so bits.size()*8 == build-time nbits always).
inline bool BloomMaybe(const char* bits, uint32_t nbits, uint32_t bits_per_key,
                       std::string_view key) {
  if (nbits == 0) return true;
  uint64_t h = BloomHash(key);
  uint32_t h1 = static_cast<uint32_t>(h);
  uint32_t h2 = static_cast<uint32_t>(h >> 32);
  uint32_t k = BloomNumHashes(bits_per_key);
  for (uint32_t j = 0; j < k; ++j) {
    uint32_t bit = (h1 + j * h2) % nbits;
    if ((static_cast<unsigned char>(bits[bit >> 3]) & (1u << (bit & 7))) == 0)
      return false;
  }
  return true;
}

}  // namespace experiment::embedded
