#pragma once

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace aerokv::test {

// Fresh, unique scratch directory removed when the object goes out of scope.
class TempDir {
 public:
  TempDir() {
    char tmpl[] = "/tmp/aerokv_test_XXXXXX";
    const char* p = ::mkdtemp(tmpl);
    if (p == nullptr) std::abort();
    path_ = p;
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

inline std::string Key(uint64_t i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%012llu", static_cast<unsigned long long>(i));
  return buf;
}

}  // namespace aerokv::test
