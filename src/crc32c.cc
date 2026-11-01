#include "crc32c.h"

#include <array>
#include <cstring>

#if defined(__x86_64__)
#include <nmmintrin.h>
#endif

namespace aerokv::crc32c {
namespace {

const std::array<uint32_t, 256>& Table() {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0x82F63B78u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  return table;
}

uint32_t Software(uint32_t crc, const char* p, size_t n) {
  const auto& t = Table();
  for (size_t i = 0; i < n; ++i) crc = t[(crc ^ static_cast<uint8_t>(p[i])) & 0xff] ^ (crc >> 8);
  return crc;
}

#if defined(__x86_64__)
__attribute__((target("sse4.2"))) uint32_t Hardware(uint32_t crc, const char* p, size_t n) {
  uint64_t c = crc;
  while (n >= 8) {
    uint64_t word;
    std::memcpy(&word, p, 8);
    c = _mm_crc32_u64(c, word);
    p += 8;
    n -= 8;
  }
  uint32_t c32 = static_cast<uint32_t>(c);
  while (n-- > 0) c32 = _mm_crc32_u8(c32, static_cast<uint8_t>(*p++));
  return c32;
}

bool HasSse42() {
  static const bool has = __builtin_cpu_supports("sse4.2");
  return has;
}
#endif

}  // namespace

uint32_t Value(const char* data, size_t n) {
#if defined(__x86_64__)
  if (HasSse42()) return Hardware(0xFFFFFFFFu, data, n) ^ 0xFFFFFFFFu;
#endif
  return Software(0xFFFFFFFFu, data, n) ^ 0xFFFFFFFFu;
}

uint32_t SoftwareValue(const char* data, size_t n) {
  return Software(0xFFFFFFFFu, data, n) ^ 0xFFFFFFFFu;
}

}  // namespace aerokv::crc32c
