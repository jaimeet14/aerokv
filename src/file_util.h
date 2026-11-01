#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "aerokv/status.h"

namespace aerokv {

Status ErrnoStatus(const std::string& context);
Status WriteAll(int fd, const char* data, size_t n);
Status ReadFileToString(const std::string& path, std::string* out);
// Writes to `path`.tmp, fsyncs, renames over `path` and fsyncs the directory.
Status AtomicWriteFile(const std::string& dir, const std::string& path, std::string_view data);
Status SyncDir(const std::string& dir);
Status ListDir(const std::string& dir, std::vector<std::string>* names);
Status CreateDirIfMissing(const std::string& dir);
Status RemoveFile(const std::string& path);
bool FileExists(const std::string& path);
// Exclusive advisory lock so that two DB instances cannot open the same directory.
Status LockFile(const std::string& path, int* fd);
void UnlockFile(int fd);

enum class FileKind { kTable, kLog, kOther };
std::string TableFileName(const std::string& dir, uint64_t number);
std::string LogFileName(const std::string& dir, uint64_t number);
FileKind ParseFileName(const std::string& name, uint64_t* number);

}  // namespace aerokv
