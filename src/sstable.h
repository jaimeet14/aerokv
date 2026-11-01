#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "aerokv/db.h"
#include "iterator.h"

namespace aerokv {

// SSTable layout:
//   [data block][crc32c] ... [index block][crc32c] [bloom filter][crc32c] [footer]
// Data block entry: varint shared | varint unshared | varint value_len | type | key suffix | value
//   (keys are prefix-compressed against the previous key in the same block)
// Index entry: len-prefixed last key of the block | fixed64 offset | fixed32 size
// Footer (40 bytes): index offset, index size, filter offset, filter size, entry count, magic

class TableBuilder {
 public:
  TableBuilder(const Options& options, std::string path);
  ~TableBuilder();
  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;

  Status Open();
  // Keys must be added in strictly increasing order.
  void Add(std::string_view key, std::string_view value, ValueType type);
  // Writes the index, filter and footer, then fsyncs the file.
  Status Finish();
  // Closes and deletes a partially written file.
  void Abandon();

  uint64_t FileSize() const { return offset_ + block_.size(); }
  uint64_t NumEntries() const { return entries_; }
  const std::string& smallest() const { return smallest_; }
  const std::string& largest() const { return largest_; }

 private:
  void FlushBlock();
  void WriteRaw(std::string_view data);

  const size_t block_size_;
  const int bloom_bits_per_key_;
  const std::string path_;
  std::FILE* file_ = nullptr;
  Status status_;
  uint64_t offset_ = 0;
  uint64_t entries_ = 0;
  std::string block_;
  std::string prev_key_;  // previous key in the current block, for prefix compression
  std::string index_;
  std::vector<uint64_t> key_hashes_;
  std::string smallest_;
  std::string largest_;
};

// Read-only view of an SSTable, memory-mapped. Block checksums are verified the
// first time each block is read. The mapping stays valid after the file is
// unlinked, so readers holding a reference are safe while compaction deletes it.
class TableReader : public std::enable_shared_from_this<TableReader> {
 public:
  enum class Lookup { kFound, kNotFound, kFilteredOut };

  static Status Open(const std::string& path, std::shared_ptr<TableReader>* out);
  ~TableReader();
  TableReader(const TableReader&) = delete;
  TableReader& operator=(const TableReader&) = delete;

  Status Get(std::string_view key, std::string* value, ValueType* type, Lookup* result) const;
  std::unique_ptr<Iterator> NewIterator() const;

  uint64_t num_entries() const { return num_entries_; }
  size_t num_blocks() const { return index_.size(); }

 private:
  friend class TableIterator;
  struct IndexEntry {
    std::string last_key;
    uint64_t offset;
    uint32_t size;
  };

  TableReader() = default;
  Status ReadBlock(size_t i, std::string_view* contents) const;
  size_t FindBlock(std::string_view key) const;  // first block whose last key >= key

  int fd_ = -1;
  const char* data_ = nullptr;
  size_t size_ = 0;
  std::vector<IndexEntry> index_;
  std::string_view filter_;
  uint64_t num_entries_ = 0;
  std::unique_ptr<std::atomic<bool>[]> verified_;
};

}  // namespace aerokv
