#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aerokv {

// Builds a bloom filter over 64-bit key hashes. k = bits_per_key * ln2 probes are
// derived by double hashing (Kirsch–Mitzenmacher), so each key is hashed only once.
// Returns an empty string when bits_per_key <= 0.
std::string BuildBloomFilter(const std::vector<uint64_t>& hashes, int bits_per_key);

// False means the key is definitely absent. An empty or malformed filter returns true.
bool BloomMayContain(std::string_view filter, uint64_t hash);

}  // namespace aerokv
