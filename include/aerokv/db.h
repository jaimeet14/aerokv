#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aerokv/status.h"

namespace aerokv {

struct Options {
  bool create_if_missing = true;
  size_t write_buffer_size = 4 << 20;            // memtable size before it is frozen and flushed
  size_t max_immutable_memtables = 2;            // frozen memtables allowed before writes stall
  size_t block_size = 4096;                      // target size of an SSTable data block
  int bloom_bits_per_key = 10;                   // 0 disables bloom filters
  int num_levels = 5;                            // L0 .. L(num_levels-1)
  int l0_compaction_trigger = 4;                 // L0 file count that schedules L0->L1
  int l0_stop_writes_trigger = 12;               // L0 file count at which writes stall
  uint64_t max_bytes_for_level_base = 10 << 20;  // L1 budget; each deeper level gets 10x
  uint64_t target_file_size = 2 << 20;           // compaction output file size
};

struct WriteOptions {
  bool sync = false;  // fdatasync the WAL before acknowledging the write
};

// A group of updates applied atomically: after a crash either all of them are
// recovered or none are.
class WriteBatch {
 public:
  void Put(std::string_view key, std::string_view value);
  void Delete(std::string_view key);
  void Clear();
  size_t Count() const { return count_; }

 private:
  friend class DBImpl;
  std::string rep_;
  size_t count_ = 0;
};

struct LevelStats {
  int files = 0;
  uint64_t bytes = 0;
};

struct DBStats {
  std::vector<LevelStats> levels;
  uint64_t user_bytes_written = 0;
  uint64_t wal_bytes_written = 0;
  uint64_t flush_bytes_written = 0;
  uint64_t compaction_bytes_read = 0;
  uint64_t compaction_bytes_written = 0;
  uint64_t flushes = 0;
  uint64_t compactions = 0;
  uint64_t trivial_moves = 0;
  uint64_t write_stalls = 0;
  uint64_t tombstones_dropped = 0;
  uint64_t table_probes = 0;     // SSTables whose key range covered a Get
  uint64_t bloom_negatives = 0;  // of those, answered by the bloom filter alone
  uint64_t recovered_records = 0;
  uint64_t wal_bytes_dropped = 0;  // torn or corrupt WAL tail discarded during recovery

  // SSTable bytes written (flush + compaction) per byte of user data.
  double WriteAmplification() const;
};

class DB {
 public:
  static Status Open(const Options& options, const std::string& path, std::unique_ptr<DB>* db);
  virtual ~DB() = default;

  Status Put(const WriteOptions& options, std::string_view key, std::string_view value);
  Status Delete(const WriteOptions& options, std::string_view key);
  virtual Status Write(const WriteOptions& options, const WriteBatch& batch) = 0;

  // Returns NotFound if the key is absent or deleted.
  virtual Status Get(std::string_view key, std::string* value) = 0;

  // Up to `limit` live entries with start <= key < end, in key order.
  // An empty `end` means no upper bound.
  virtual Status Scan(std::string_view start, std::string_view end, size_t limit,
                      std::vector<std::pair<std::string, std::string>>* out) = 0;

  // Freezes the memtable and blocks until it has been written to an SSTable.
  virtual Status Flush() = 0;
  // Blocks until no flush or compaction is pending.
  virtual Status WaitForCompactions() = 0;
  virtual DBStats GetStats() = 0;
};

}  // namespace aerokv
