#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "aerokv/status.h"

namespace aerokv {

enum class ValueType : uint8_t { kDeletion = 0, kValue = 1 };

struct Entry {
  std::string key;
  std::string value;
  ValueType type = ValueType::kValue;
};

// Sorted cursor over (key, value, type). key()/value() are valid until the next move.
class Iterator {
 public:
  virtual ~Iterator() = default;
  virtual bool Valid() const = 0;
  virtual void SeekToFirst() = 0;
  virtual void Seek(std::string_view target) = 0;  // first entry with key >= target
  virtual void Next() = 0;
  virtual std::string_view key() const = 0;
  virtual std::string_view value() const = 0;
  virtual ValueType type() const = 0;
  virtual Status status() const { return Status::OK(); }
};

std::unique_ptr<Iterator> NewVectorIterator(std::vector<Entry> sorted_entries);

// K-way merge over sorted children using a min-heap. children[0] is the newest
// source: when several children hold the same key, only the entry from the
// lowest-indexed child is returned (tombstones included).
std::unique_ptr<Iterator> NewMergingIterator(std::vector<std::unique_ptr<Iterator>> children);

}  // namespace aerokv
