#include "sstable.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

#include "bloom.h"
#include "coding.h"
#include "crc32c.h"
#include "file_util.h"

namespace aerokv {
namespace {

constexpr uint64_t kTableMagic = 0x41455230534B5631ULL;
constexpr size_t kFooterSize = 40;

// Walks the prefix-compressed entries of one data block.
class BlockCursor {
 public:
  explicit BlockCursor(std::string_view block) : rest_(block) {}

  // Moves to the next entry; false at the end of the block or on malformed data.
  bool Next() {
    if (rest_.empty()) return false;
    uint32_t shared = 0, unshared = 0, value_len = 0;
    if (!GetVarint32(&rest_, &shared) || !GetVarint32(&rest_, &unshared) ||
        !GetVarint32(&rest_, &value_len) || shared > key_.size() ||
        rest_.size() < 1ULL + unshared + value_len) {
      corrupt_ = true;
      return false;
    }
    const uint8_t type = static_cast<uint8_t>(rest_[0]);
    if (type > static_cast<uint8_t>(ValueType::kValue)) {
      corrupt_ = true;
      return false;
    }
    type_ = static_cast<ValueType>(type);
    rest_.remove_prefix(1);
    key_.resize(shared);
    key_.append(rest_.data(), unshared);
    rest_.remove_prefix(unshared);
    value_ = rest_.substr(0, value_len);
    rest_.remove_prefix(value_len);
    return true;
  }

  const std::string& key() const { return key_; }
  std::string_view value() const { return value_; }
  ValueType type() const { return type_; }
  bool corrupt() const { return corrupt_; }

 private:
  std::string_view rest_;
  std::string key_;
  std::string_view value_;
  ValueType type_ = ValueType::kValue;
  bool corrupt_ = false;
};

bool CheckedRegion(const char* base, size_t file_size, uint64_t offset, uint64_t size,
                   std::string_view* out) {
  if (offset > file_size || size + 4 > file_size - offset) return false;
  const char* p = base + offset;
  if (crc32c::Value(p, size) != DecodeFixed32(p + size)) return false;
  *out = std::string_view(p, size);
  return true;
}

}  // namespace

// ---------------------------------------------------------------- builder

TableBuilder::TableBuilder(const Options& options, std::string path)
    : block_size_(options.block_size),
      bloom_bits_per_key_(options.bloom_bits_per_key),
      path_(std::move(path)) {}

TableBuilder::~TableBuilder() {
  if (file_ != nullptr) std::fclose(file_);
}

Status TableBuilder::Open() {
  file_ = std::fopen(path_.c_str(), "wbe");
  if (file_ == nullptr) return ErrnoStatus(path_);
  std::setvbuf(file_, nullptr, _IOFBF, 1 << 20);
  return Status::OK();
}

void TableBuilder::WriteRaw(std::string_view data) {
  if (!status_.ok()) return;
  if (std::fwrite(data.data(), 1, data.size(), file_) != data.size()) {
    status_ = ErrnoStatus("write " + path_);
  }
  offset_ += data.size();
}

void TableBuilder::Add(std::string_view key, std::string_view value, ValueType type) {
  if (entries_ == 0) smallest_.assign(key);
  size_t shared = 0;
  const size_t limit = std::min(prev_key_.size(), key.size());
  while (shared < limit && prev_key_[shared] == key[shared]) ++shared;

  PutVarint32(&block_, static_cast<uint32_t>(shared));
  PutVarint32(&block_, static_cast<uint32_t>(key.size() - shared));
  PutVarint32(&block_, static_cast<uint32_t>(value.size()));
  block_.push_back(static_cast<char>(type));
  block_.append(key.data() + shared, key.size() - shared);
  block_.append(value.data(), value.size());

  prev_key_.assign(key);
  largest_.assign(key);
  key_hashes_.push_back(Hash64(key));
  ++entries_;
  if (block_.size() >= block_size_) FlushBlock();
}

void TableBuilder::FlushBlock() {
  if (block_.empty()) return;
  const uint64_t offset = offset_;
  const uint32_t size = static_cast<uint32_t>(block_.size());
  PutFixed32(&block_, crc32c::Value(block_.data(), size));
  WriteRaw(block_);

  PutLengthPrefixed(&index_, largest_);
  PutFixed64(&index_, offset);
  PutFixed32(&index_, size);
  block_.clear();
  prev_key_.clear();
}

Status TableBuilder::Finish() {
  FlushBlock();

  const uint64_t index_offset = offset_;
  const uint32_t index_size = static_cast<uint32_t>(index_.size());
  PutFixed32(&index_, crc32c::Value(index_.data(), index_size));
  WriteRaw(index_);

  std::string filter = BuildBloomFilter(key_hashes_, bloom_bits_per_key_);
  const uint64_t filter_offset = offset_;
  const uint32_t filter_size = static_cast<uint32_t>(filter.size());
  PutFixed32(&filter, crc32c::Value(filter.data(), filter_size));
  WriteRaw(filter);

  std::string footer;
  PutFixed64(&footer, index_offset);
  PutFixed32(&footer, index_size);
  PutFixed64(&footer, filter_offset);
  PutFixed32(&footer, filter_size);
  PutFixed64(&footer, entries_);
  PutFixed64(&footer, kTableMagic);
  WriteRaw(footer);

  if (status_.ok() && std::fflush(file_) != 0) status_ = ErrnoStatus("flush " + path_);
  if (status_.ok() && ::fsync(fileno(file_)) != 0) status_ = ErrnoStatus("fsync " + path_);
  if (std::fclose(file_) != 0 && status_.ok()) status_ = ErrnoStatus("close " + path_);
  file_ = nullptr;
  return status_;
}

void TableBuilder::Abandon() {
  if (file_ != nullptr) {
    std::fclose(file_);
    file_ = nullptr;
  }
  RemoveFile(path_);
}

// ---------------------------------------------------------------- reader

Status TableReader::Open(const std::string& path, std::shared_ptr<TableReader>* out) {
  std::shared_ptr<TableReader> t(new TableReader());
  t->fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (t->fd_ < 0) return ErrnoStatus(path);
  struct stat st {};
  if (::fstat(t->fd_, &st) != 0) return ErrnoStatus(path);
  t->size_ = static_cast<size_t>(st.st_size);
  if (t->size_ < kFooterSize) return Status::Corruption(path + ": file too short");

  void* map = ::mmap(nullptr, t->size_, PROT_READ, MAP_SHARED, t->fd_, 0);
  if (map == MAP_FAILED) return ErrnoStatus("mmap " + path);
  t->data_ = static_cast<const char*>(map);

  const char* footer = t->data_ + t->size_ - kFooterSize;
  if (DecodeFixed64(footer + 32) != kTableMagic) return Status::Corruption(path + ": bad magic");
  const uint64_t index_offset = DecodeFixed64(footer);
  const uint32_t index_size = DecodeFixed32(footer + 8);
  const uint64_t filter_offset = DecodeFixed64(footer + 12);
  const uint32_t filter_size = DecodeFixed32(footer + 20);
  t->num_entries_ = DecodeFixed64(footer + 24);

  const size_t body = t->size_ - kFooterSize;
  std::string_view index;
  if (!CheckedRegion(t->data_, body, index_offset, index_size, &index)) {
    return Status::Corruption(path + ": bad index block");
  }
  if (!CheckedRegion(t->data_, body, filter_offset, filter_size, &t->filter_)) {
    return Status::Corruption(path + ": bad filter block");
  }

  while (!index.empty()) {
    std::string_view last_key;
    if (!GetLengthPrefixed(&index, &last_key) || index.size() < 12) {
      return Status::Corruption(path + ": malformed index");
    }
    IndexEntry e{std::string(last_key), DecodeFixed64(index.data()), DecodeFixed32(index.data() + 8)};
    index.remove_prefix(12);
    if (e.offset > index_offset || e.size + 4ULL > index_offset - e.offset) {
      return Status::Corruption(path + ": index points outside data region");
    }
    t->index_.push_back(std::move(e));
  }
  t->verified_ = std::make_unique<std::atomic<bool>[]>(t->index_.size());
  for (size_t i = 0; i < t->index_.size(); ++i) t->verified_[i].store(false);
  *out = std::move(t);
  return Status::OK();
}

TableReader::~TableReader() {
  if (data_ != nullptr) ::munmap(const_cast<char*>(data_), size_);
  if (fd_ >= 0) ::close(fd_);
}

Status TableReader::ReadBlock(size_t i, std::string_view* contents) const {
  const IndexEntry& e = index_[i];
  const char* p = data_ + e.offset;
  if (!verified_[i].load(std::memory_order_acquire)) {
    if (crc32c::Value(p, e.size) != DecodeFixed32(p + e.size)) {
      return Status::Corruption("block checksum mismatch");
    }
    verified_[i].store(true, std::memory_order_release);
  }
  *contents = std::string_view(p, e.size);
  return Status::OK();
}

size_t TableReader::FindBlock(std::string_view key) const {
  auto it = std::lower_bound(index_.begin(), index_.end(), key,
                             [](const IndexEntry& e, std::string_view k) {
                               return std::string_view(e.last_key) < k;
                             });
  return static_cast<size_t>(it - index_.begin());
}

Status TableReader::Get(std::string_view key, std::string* value, ValueType* type,
                        Lookup* result) const {
  *result = Lookup::kNotFound;
  if (!filter_.empty() && !BloomMayContain(filter_, Hash64(key))) {
    *result = Lookup::kFilteredOut;
    return Status::OK();
  }
  const size_t i = FindBlock(key);
  if (i == index_.size()) return Status::OK();
  std::string_view block;
  Status s = ReadBlock(i, &block);
  if (!s.ok()) return s;

  BlockCursor c(block);
  while (c.Next()) {
    const int r = std::string_view(c.key()).compare(key);
    if (r > 0) break;
    if (r == 0) {
      *type = c.type();
      if (c.type() == ValueType::kValue) value->assign(c.value());
      *result = Lookup::kFound;
      return Status::OK();
    }
  }
  return c.corrupt() ? Status::Corruption("malformed data block") : Status::OK();
}

class TableIterator final : public Iterator {
 public:
  explicit TableIterator(std::shared_ptr<const TableReader> t) : t_(std::move(t)) {
    block_ = t_->index_.size();
  }

  bool Valid() const override { return block_ < t_->index_.size() && pos_ < slots_.size(); }

  void SeekToFirst() override {
    LoadBlock(0);
    SkipEmptyBlocks();
  }

  void Seek(std::string_view target) override {
    LoadBlock(t_->FindBlock(target));
    if (block_ >= t_->index_.size()) return;
    size_t lo = 0, hi = slots_.size();
    while (lo < hi) {
      const size_t mid = (lo + hi) / 2;
      if (KeyAt(mid) < target) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    pos_ = lo;
    SkipEmptyBlocks();
  }

  void Next() override {
    ++pos_;
    SkipEmptyBlocks();
  }

  std::string_view key() const override { return KeyAt(pos_); }
  std::string_view value() const override { return slots_[pos_].value; }
  ValueType type() const override { return slots_[pos_].type; }
  Status status() const override { return status_; }

 private:
  // Keys are rebuilt from prefix compression into one arena; values point into the mapping.
  struct Slot {
    size_t key_offset;
    size_t key_size;
    std::string_view value;
    ValueType type;
  };

  std::string_view KeyAt(size_t i) const {
    return std::string_view(keys_.data() + slots_[i].key_offset, slots_[i].key_size);
  }

  void LoadBlock(size_t i) {
    block_ = i;
    pos_ = 0;
    slots_.clear();
    keys_.clear();
    if (i >= t_->index_.size()) return;
    std::string_view contents;
    Status s = t_->ReadBlock(i, &contents);
    BlockCursor c(contents);
    while (s.ok() && c.Next()) {
      slots_.push_back({keys_.size(), c.key().size(), c.value(), c.type()});
      keys_.append(c.key());
    }
    if (s.ok() && c.corrupt()) s = Status::Corruption("malformed data block");
    if (!s.ok()) {
      status_ = s;
      block_ = t_->index_.size();  // stop iteration
      slots_.clear();
    }
  }

  void SkipEmptyBlocks() {
    while (block_ < t_->index_.size() && pos_ >= slots_.size()) LoadBlock(block_ + 1);
  }

  std::shared_ptr<const TableReader> t_;
  size_t block_;
  size_t pos_ = 0;
  std::vector<Slot> slots_;
  std::string keys_;
  Status status_;
};

std::unique_ptr<Iterator> TableReader::NewIterator() const {
  return std::make_unique<TableIterator>(shared_from_this());
}

}  // namespace aerokv
