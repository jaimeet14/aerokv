#pragma once

#include <cstddef>
#include <cstdint>

namespace aerokv::crc32c {

// CRC-32C (Castagnoli). Uses the SSE4.2 instruction when the CPU has it.
uint32_t Value(const char* data, size_t n);

// Portable table-driven implementation, exposed so tests can cross-check.
uint32_t SoftwareValue(const char* data, size_t n);

}  // namespace aerokv::crc32c
