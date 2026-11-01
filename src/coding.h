#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace aerokv {

// All integers are stored little-endian, independent of host byte order.

inline void PutFixed32(std::string* dst, uint32_t v) {
  char buf[4];
  for (int i = 0; i < 4; ++i) buf[i] = static_cast<char>((v >> (8 * i)) & 0xff);
  dst->append(buf, 4);
}

inline void PutFixed64(std::string* dst, uint64_t v) {
  char buf[8];
  for (int i = 0; i < 8; ++i) buf[i] = static_cast<char>((v >> (8 * i)) & 0xff);
  dst->append(buf, 8);
}

inline uint32_t DecodeFixed32(const char* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<uint8_t>(p[i])) << (8 * i);
  return v;
}

inline uint64_t DecodeFixed64(const char* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(static_cast<uint8_t>(p[i])) << (8 * i);
  return v;
}

inline void PutVarint32(std::string* dst, uint32_t v) {
  while (v >= 0x80) {
    dst->push_back(static_cast<char>(v | 0x80));
    v >>= 7;
  }
  dst->push_back(static_cast<char>(v));
}

inline bool GetVarint32(std::string_view* in, uint32_t* v) {
  uint32_t result = 0;
  for (int shift = 0; shift <= 28 && !in->empty(); shift += 7) {
    const uint8_t byte = static_cast<uint8_t>(in->front());
    in->remove_prefix(1);
    result |= static_cast<uint32_t>(byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) {
      *v = result;
      return true;
    }
  }
  return false;
}

inline void PutLengthPrefixed(std::string* dst, std::string_view s) {
  PutVarint32(dst, static_cast<uint32_t>(s.size()));
  dst->append(s.data(), s.size());
}

inline bool GetLengthPrefixed(std::string_view* in, std::string_view* out) {
  uint32_t n = 0;
  if (!GetVarint32(in, &n) || in->size() < n) return false;
  *out = in->substr(0, n);
  in->remove_prefix(n);
  return true;
}

// FNV-1a followed by the MurmurHash3 finalizer for good avalanche on short keys.
inline uint64_t Hash64(std::string_view s) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ULL;
  h ^= h >> 33;
  return h;
}

}  // namespace aerokv
