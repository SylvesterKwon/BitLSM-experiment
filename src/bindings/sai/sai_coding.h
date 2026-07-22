#pragma once
// Little-endian fixed-width serialization helpers for the SAI baseline blob.
// Local (not RocksDB util/coding.h) so pure components unit-test without
// RocksDB headers.
#include <cstdint>
#include <cstring>
#include <string>

namespace experiment::sai {

inline void PutU16(std::string& out, uint16_t v) { out.append(reinterpret_cast<const char*>(&v), 2); }
inline void PutU32(std::string& out, uint32_t v) { out.append(reinterpret_cast<const char*>(&v), 4); }
inline void PutU64(std::string& out, uint64_t v) { out.append(reinterpret_cast<const char*>(&v), 8); }
inline void PutF64(std::string& out, double v)   { out.append(reinterpret_cast<const char*>(&v), 8); }
inline uint16_t GetU16(const char* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
inline uint32_t GetU32(const char* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint64_t GetU64(const char* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
inline double   GetF64(const char* p) { double v;   std::memcpy(&v, p, 8); return v; }
// Overwrite a u32 previously reserved at offset `at` (for back-patching).
inline void PatchU32(std::string& out, size_t at, uint32_t v) { std::memcpy(out.data() + at, &v, 4); }

}  // namespace experiment::sai
