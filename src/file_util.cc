#include "file_util.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace aerokv {

Status ErrnoStatus(const std::string& context) {
  return Status::IOError(context + ": " + std::strerror(errno));
}

Status WriteAll(int fd, const char* data, size_t n) {
  while (n > 0) {
    const ssize_t w = ::write(fd, data, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return ErrnoStatus("write");
    }
    data += w;
    n -= static_cast<size_t>(w);
  }
  return Status::OK();
}

Status ReadFileToString(const std::string& path, std::string* out) {
  out->clear();
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return ErrnoStatus(path);
  char buf[1 << 16];
  while (true) {
    const ssize_t r = ::read(fd, buf, sizeof(buf));
    if (r < 0) {
      if (errno == EINTR) continue;
      Status s = ErrnoStatus(path);
      ::close(fd);
      return s;
    }
    if (r == 0) break;
    out->append(buf, static_cast<size_t>(r));
  }
  ::close(fd);
  return Status::OK();
}

Status SyncDir(const std::string& dir) {
  const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return ErrnoStatus(dir);
  Status s;
  if (::fsync(fd) != 0) s = ErrnoStatus("fsync " + dir);
  ::close(fd);
  return s;
}

Status AtomicWriteFile(const std::string& dir, const std::string& path, std::string_view data) {
  const std::string tmp = path + ".tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return ErrnoStatus(tmp);
  Status s = WriteAll(fd, data.data(), data.size());
  if (s.ok() && ::fsync(fd) != 0) s = ErrnoStatus("fsync " + tmp);
  ::close(fd);
  if (s.ok() && ::rename(tmp.c_str(), path.c_str()) != 0) s = ErrnoStatus("rename " + tmp);
  if (s.ok()) s = SyncDir(dir);
  if (!s.ok()) ::unlink(tmp.c_str());
  return s;
}

Status ListDir(const std::string& dir, std::vector<std::string>* names) {
  names->clear();
  DIR* d = ::opendir(dir.c_str());
  if (d == nullptr) return ErrnoStatus(dir);
  while (struct dirent* e = ::readdir(d)) {
    if (std::strcmp(e->d_name, ".") != 0 && std::strcmp(e->d_name, "..") != 0) {
      names->emplace_back(e->d_name);
    }
  }
  ::closedir(d);
  return Status::OK();
}

Status CreateDirIfMissing(const std::string& dir) {
  if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) return ErrnoStatus("mkdir " + dir);
  return Status::OK();
}

Status RemoveFile(const std::string& path) {
  if (::unlink(path.c_str()) != 0 && errno != ENOENT) return ErrnoStatus("unlink " + path);
  return Status::OK();
}

bool FileExists(const std::string& path) { return ::access(path.c_str(), F_OK) == 0; }

Status LockFile(const std::string& path, int* fd) {
  *fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (*fd < 0) return ErrnoStatus(path);
  if (::flock(*fd, LOCK_EX | LOCK_NB) != 0) {
    Status s = Status::IOError("database is locked by another instance: " + path);
    ::close(*fd);
    *fd = -1;
    return s;
  }
  return Status::OK();
}

void UnlockFile(int fd) {
  if (fd >= 0) {
    ::flock(fd, LOCK_UN);
    ::close(fd);
  }
}

namespace {
std::string NumberedName(const std::string& dir, uint64_t number, const char* suffix) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "/%06llu.%s", static_cast<unsigned long long>(number), suffix);
  return dir + buf;
}
}  // namespace

std::string TableFileName(const std::string& dir, uint64_t number) {
  return NumberedName(dir, number, "sst");
}

std::string LogFileName(const std::string& dir, uint64_t number) {
  return NumberedName(dir, number, "log");
}

FileKind ParseFileName(const std::string& name, uint64_t* number) {
  const size_t dot = name.find('.');
  if (dot == std::string::npos || dot == 0) return FileKind::kOther;
  uint64_t n = 0;
  for (size_t i = 0; i < dot; ++i) {
    if (name[i] < '0' || name[i] > '9') return FileKind::kOther;
    n = n * 10 + static_cast<uint64_t>(name[i] - '0');
  }
  const std::string suffix = name.substr(dot + 1);
  *number = n;
  if (suffix == "sst") return FileKind::kTable;
  if (suffix == "log") return FileKind::kLog;
  return FileKind::kOther;
}

}  // namespace aerokv
