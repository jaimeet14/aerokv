#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>

#include "iterator.h"

namespace aerokv {

// In-memory write buffer: a skiplist ordered by key holding the newest value or
// tombstone for each key. Writers take an exclusive lock; readers and iterators
// share it, so a frozen memtable can be read without contention.
class MemTable : public std::enable_shared_from_this<MemTable> {
 public:
  explicit MemTable(uint64_t log_number);
  ~MemTable();
  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  void Add(std::string_view key, std::string_view value, ValueType type);

  // True if the key is present as a value or a tombstone.
  bool Get(std::string_view key, std::string* value, ValueType* type) const;

  // Holds a shared lock for its lifetime: writers to this memtable wait until it is destroyed.
  std::unique_ptr<Iterator> NewIterator() const;

  size_t ApproximateMemoryUsage() const { return usage_.load(std::memory_order_relaxed); }
  size_t Count() const;
  uint64_t log_number() const { return log_number_; }

 private:
  static constexpr int kMaxHeight = 12;
  struct Node {
    std::string key;
    std::string value;
    ValueType type;
    Node* next[kMaxHeight];
  };
  class Iter;

  // First node with key >= target; fills prev[] with the rightmost node before it per level.
  Node* FindGreaterOrEqual(std::string_view target, Node** prev) const;
  int RandomHeight();

  mutable std::shared_mutex mu_;
  Node* const head_;
  int height_ = 1;
  uint64_t rng_ = 0x9E3779B97F4A7C15ULL;
  size_t count_ = 0;
  std::atomic<size_t> usage_{0};
  const uint64_t log_number_;
};

}  // namespace aerokv
