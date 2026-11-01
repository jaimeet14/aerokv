#pragma once

#include <string>
#include <utility>

namespace aerokv {

class Status {
 public:
  Status() = default;

  static Status OK() { return Status(); }
  static Status NotFound(std::string msg = "") { return Status(kNotFound, std::move(msg)); }
  static Status Corruption(std::string msg) { return Status(kCorruption, std::move(msg)); }
  static Status IOError(std::string msg) { return Status(kIOError, std::move(msg)); }
  static Status InvalidArgument(std::string msg) { return Status(kInvalidArgument, std::move(msg)); }

  bool ok() const { return code_ == kOk; }
  bool IsNotFound() const { return code_ == kNotFound; }
  bool IsCorruption() const { return code_ == kCorruption; }
  bool IsIOError() const { return code_ == kIOError; }
  bool IsInvalidArgument() const { return code_ == kInvalidArgument; }

  std::string ToString() const {
    static const char* const kNames[] = {"OK", "NotFound", "Corruption", "IOError",
                                         "InvalidArgument"};
    std::string r = kNames[code_];
    if (!msg_.empty()) {
      r += ": ";
      r += msg_;
    }
    return r;
  }

 private:
  enum Code { kOk, kNotFound, kCorruption, kIOError, kInvalidArgument };
  Status(Code code, std::string msg) : code_(code), msg_(std::move(msg)) {}

  Code code_ = kOk;
  std::string msg_;
};

}  // namespace aerokv
