#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "aerokv/status.h"

namespace aerokv {

// Write-ahead log. Each record is [crc32c of len+payload : 4][len : 4][payload].
class LogWriter {
 public:
  static Status Open(const std::string& path, std::unique_ptr<LogWriter>* out);
  ~LogWriter();
  LogWriter(const LogWriter&) = delete;
  LogWriter& operator=(const LogWriter&) = delete;

  // One write(2) per record, so an acknowledged record survives a process crash;
  // `sync` adds fdatasync so it also survives power loss.
  Status AddRecord(std::string_view payload, bool sync);

 private:
  explicit LogWriter(int fd) : fd_(fd) {}
  int fd_;
  std::string buf_;
};

// Calls `fn` for every intact record in order. Reading stops at the first torn or
// corrupt record; the number of bytes skipped from there to the end is returned
// in `dropped_bytes`.
Status ReadLog(const std::string& path, const std::function<Status(std::string_view)>& fn,
               uint64_t* dropped_bytes);

}  // namespace aerokv
