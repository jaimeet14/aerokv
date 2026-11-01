#include "bloom.h"

#include <algorithm>

namespace aerokv {
namespace {

constexpr int kMaxProbes = 30;

}  // namespace

std::string BuildBloomFilter(const std::vector<uint64_t>& hashes, int bits_per_key) {
  if (bits_per_key <= 0 || hashes.empty()) return "";
  size_t bits = std::max<size_t>(64, hashes.size() * static_cast<size_t>(bits_per_key));
  const size_t bytes = (bits + 7) / 8;
  bits = bytes * 8;
  const int k = std::clamp(static_cast<int>(bits_per_key * 0.69), 1, kMaxProbes);

  std::string filter(bytes, '\0');
  for (uint64_t h : hashes) {
    const uint64_t h1 = static_cast<uint32_t>(h);
    const uint64_t h2 = h >> 32;
    for (int j = 0; j < k; ++j) {
      const uint64_t pos = (h1 + static_cast<uint64_t>(j) * h2) % bits;
      filter[pos / 8] = static_cast<char>(filter[pos / 8] | (1 << (pos % 8)));
    }
  }
  filter.push_back(static_cast<char>(k));
  return filter;
}

bool BloomMayContain(std::string_view filter, uint64_t hash) {
  if (filter.size() < 2) return true;
  const int k = static_cast<uint8_t>(filter.back());
  if (k < 1 || k > kMaxProbes) return true;
  const uint64_t bits = (filter.size() - 1) * 8;
  const uint64_t h1 = static_cast<uint32_t>(hash);
  const uint64_t h2 = hash >> 32;
  for (int j = 0; j < k; ++j) {
    const uint64_t pos = (h1 + static_cast<uint64_t>(j) * h2) % bits;
    if ((static_cast<uint8_t>(filter[pos / 8]) & (1 << (pos % 8))) == 0) return false;
  }
  return true;
}

}  // namespace aerokv
