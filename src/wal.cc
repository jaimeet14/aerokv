#include "wal.h"

#include <fcntl.h>
#include <unistd.h>

#include "coding.h"
#include "crc32c.h"
#include "file_util.h"

namespace aerokv {

namespace {
constexpr size_t kHeaderSize = 8;
}  // namespace

Status LogWriter::Open(const std::string& path, std::unique_ptr<LogWriter>* out) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
  if (fd < 0) return ErrnoStatus(path);
  out->reset(new LogWriter(fd));
  return Status::OK();
}

LogWriter::~LogWriter() {
  if (fd_ >= 0) ::close(fd_);
}

Status LogWriter::AddRecord(std::string_view payload, bool sync) {
  buf_.clear();
  PutFixed32(&buf_, 0);  // checksum placeholder
  PutFixed32(&buf_, static_cast<uint32_t>(payload.size()));
  buf_.append(payload.data(), payload.size());
  const uint32_t crc = crc32c::Value(buf_.data() + 4, buf_.size() - 4);
  for (int i = 0; i < 4; ++i) buf_[i] = static_cast<char>((crc >> (8 * i)) & 0xff);

  Status s = WriteAll(fd_, buf_.data(), buf_.size());
  if (s.ok() && sync && ::fdatasync(fd_) != 0) s = ErrnoStatus("fdatasync");
  return s;
}

Status ReadLog(const std::string& path, const std::function<Status(std::string_view)>& fn,
               uint64_t* dropped_bytes) {
  std::string data;
  Status s = ReadFileToString(path, &data);
  if (!s.ok()) return s;

  size_t pos = 0;
  while (data.size() - pos >= kHeaderSize) {
    const uint32_t crc = DecodeFixed32(data.data() + pos);
    const uint32_t len = DecodeFixed32(data.data() + pos + 4);
    if (data.size() - pos - kHeaderSize < len) break;  // torn write
    if (crc32c::Value(data.data() + pos + 4, 4 + static_cast<size_t>(len)) != crc) break;
    s = fn(std::string_view(data.data() + pos + kHeaderSize, len));
    if (!s.ok()) return s;
    pos += kHeaderSize + len;
  }
  *dropped_bytes = data.size() - pos;
  return Status::OK();
}

}  // namespace aerokv
