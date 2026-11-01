#include "aerokv/db.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_set>

#include "coding.h"
#include "crc32c.h"
#include "file_util.h"
#include "memtable.h"
#include "sstable.h"
#include "wal.h"

namespace aerokv {

// ---------------------------------------------------------------- write batch

void WriteBatch::Put(std::string_view key, std::string_view value) {
  rep_.push_back(static_cast<char>(ValueType::kValue));
  PutLengthPrefixed(&rep_, key);
  PutLengthPrefixed(&rep_, value);
  ++count_;
}

void WriteBatch::Delete(std::string_view key) {
  rep_.push_back(static_cast<char>(ValueType::kDeletion));
  PutLengthPrefixed(&rep_, key);
  ++count_;
}

void WriteBatch::Clear() {
  rep_.clear();
  count_ = 0;
}

double DBStats::WriteAmplification() const {
  if (user_bytes_written == 0) return 0;
  return static_cast<double>(flush_bytes_written + compaction_bytes_written) /
         static_cast<double>(user_bytes_written);
}

Status DB::Put(const WriteOptions& options, std::string_view key, std::string_view value) {
  WriteBatch b;
  b.Put(key, value);
  return Write(options, b);
}

Status DB::Delete(const WriteOptions& options, std::string_view key) {
  WriteBatch b;
  b.Delete(key);
  return Write(options, b);
}

namespace {

constexpr uint64_t kManifestMagic = 0x414552304D414E31ULL;

Status ApplyBatch(std::string_view rep, MemTable* mem, uint64_t* applied) {
  while (!rep.empty()) {
    const auto type = static_cast<ValueType>(rep.front());
    rep.remove_prefix(1);
    std::string_view key, value;
    if (!GetLengthPrefixed(&rep, &key)) return Status::Corruption("malformed batch key");
    if (type == ValueType::kValue) {
      if (!GetLengthPrefixed(&rep, &value)) return Status::Corruption("malformed batch value");
    } else if (type != ValueType::kDeletion) {
      return Status::Corruption("unknown batch entry type");
    }
    mem->Add(key, value, type);
    ++*applied;
  }
  return Status::OK();
}

struct FileMeta {
  uint64_t number = 0;
  uint64_t file_size = 0;
  uint64_t num_entries = 0;
  std::string smallest;
  std::string largest;
  std::shared_ptr<TableReader> table;
};
using FilePtr = std::shared_ptr<FileMeta>;

// Immutable snapshot of the LSM tree's files. L0 is ordered newest first and its
// files may overlap; L1+ are sorted by key and non-overlapping.
struct Version {
  std::vector<std::vector<FilePtr>> levels;
};

bool Overlaps(const FileMeta& f, std::string_view smallest, std::string_view largest) {
  return !(std::string_view(f.largest) < smallest || std::string_view(f.smallest) > largest);
}

void SortLevel(std::vector<FilePtr>* files) {
  std::sort(files->begin(), files->end(),
            [](const FilePtr& a, const FilePtr& b) { return a->smallest < b->smallest; });
}

// Concatenates the non-overlapping files of one level, opening each lazily.
class LevelIterator final : public Iterator {
 public:
  explicit LevelIterator(std::vector<FilePtr> files) : files_(std::move(files)) {}

  bool Valid() const override { return cur_ != nullptr && cur_->Valid(); }

  void SeekToFirst() override {
    Open(0);
    if (cur_) cur_->SeekToFirst();
    SkipExhausted();
  }

  void Seek(std::string_view target) override {
    auto it = std::lower_bound(files_.begin(), files_.end(), target,
                               [](const FilePtr& f, std::string_view t) {
                                 return std::string_view(f->largest) < t;
                               });
    Open(static_cast<size_t>(it - files_.begin()));
    if (cur_) cur_->Seek(target);
    SkipExhausted();
  }

  void Next() override {
    cur_->Next();
    SkipExhausted();
  }

  std::string_view key() const override { return cur_->key(); }
  std::string_view value() const override { return cur_->value(); }
  ValueType type() const override { return cur_->type(); }
  Status status() const override { return status_; }

 private:
  void Open(size_t i) {
    idx_ = i;
    cur_ = i < files_.size() ? files_[i]->table->NewIterator() : nullptr;
  }

  void SkipExhausted() {
    while (cur_ != nullptr && !cur_->Valid()) {
      if (!cur_->status().ok()) {
        status_ = cur_->status();
        cur_.reset();
        return;
      }
      Open(idx_ + 1);
      if (cur_) cur_->SeekToFirst();
    }
  }

  std::vector<FilePtr> files_;
  size_t idx_ = 0;
  std::unique_ptr<Iterator> cur_;
  Status status_;
};

}  // namespace

// ---------------------------------------------------------------- DBImpl

class DBImpl final : public DB {
 public:
  DBImpl(const Options& options, std::string dir)
      : opt_(options), dir_(std::move(dir)), compact_pointer_(options.num_levels) {}
  ~DBImpl() override;

  Status Recover();

  Status Write(const WriteOptions& options, const WriteBatch& batch) override;
  Status Get(std::string_view key, std::string* value) override;
  Status Scan(std::string_view start, std::string_view end, size_t limit,
              std::vector<std::pair<std::string, std::string>>* out) override;
  Status Flush() override;
  Status WaitForCompactions() override;
  DBStats GetStats() override;

 private:
  struct Compaction {
    int level = 0;
    std::vector<FilePtr> inputs[2];  // files from `level` and `level + 1`
    bool bottommost = false;         // nothing deeper overlaps, so tombstones can be dropped
  };

  Status MakeRoomForWrite(std::unique_lock<std::mutex>& lock, bool force);
  void BackgroundLoop();
  Status FlushOldestImmutable(std::unique_lock<std::mutex>& lock);
  Status RunCompaction(std::unique_lock<std::mutex>& lock, const Compaction& c);
  bool PickCompaction(Compaction* c);
  bool NeedsCompaction() const;
  double LevelScore(const Version& v, int level) const;
  uint64_t MaxBytesForLevel(int level) const;
  uint64_t OldestLiveLog() const;  // requires mu_

  // Writes the iterator's remaining entries into one new SSTable (no lock needed).
  Status WriteTable(Iterator* it, FilePtr* out);
  Status FinishTable(uint64_t number, TableBuilder* b, FilePtr* out);
  Status WriteManifest(const Version& v, uint64_t log_number);

  const Options opt_;
  const std::string dir_;
  int lock_fd_ = -1;

  std::mutex mu_;
  std::condition_variable bg_cv_;    // wakes the background thread
  std::condition_variable done_cv_;  // signalled when background work finishes
  std::shared_ptr<MemTable> mem_;
  std::vector<std::shared_ptr<MemTable>> imm_;  // frozen memtables, newest first
  std::unique_ptr<LogWriter> log_;
  std::shared_ptr<const Version> current_;
  std::vector<std::string> compact_pointer_;  // round-robin cursor per level
  std::atomic<uint64_t> next_file_number_{1};
  bool bg_busy_ = false;
  bool shutting_down_ = false;
  Status bg_error_;
  std::thread bg_thread_;

  DBStats stats_;  // guarded by mu_
  std::atomic<uint64_t> table_probes_{0};
  std::atomic<uint64_t> bloom_negatives_{0};
};

Status DB::Open(const Options& options, const std::string& path, std::unique_ptr<DB>* db) {
  if (options.num_levels < 2 || options.l0_compaction_trigger < 1 ||
      options.l0_stop_writes_trigger < options.l0_compaction_trigger ||
      options.max_immutable_memtables < 1 || options.write_buffer_size == 0 ||
      options.block_size == 0) {
    return Status::InvalidArgument("invalid options");
  }
  auto impl = std::make_unique<DBImpl>(options, path);
  Status s = impl->Recover();
  if (!s.ok()) return s;
  *db = std::move(impl);
  return Status::OK();
}

DBImpl::~DBImpl() {
  {
    std::lock_guard<std::mutex> l(mu_);
    shutting_down_ = true;
  }
  bg_cv_.notify_all();
  if (bg_thread_.joinable()) bg_thread_.join();
  log_.reset();
  UnlockFile(lock_fd_);
}

// ---------------------------------------------------------------- recovery

Status DBImpl::Recover() {
  if (!FileExists(dir_)) {
    if (!opt_.create_if_missing) return Status::InvalidArgument(dir_ + " does not exist");
    Status s = CreateDirIfMissing(dir_);
    if (!s.ok()) return s;
  }
  Status s = LockFile(dir_ + "/LOCK", &lock_fd_);
  if (!s.ok()) return s;

  std::unique_lock<std::mutex> lock(mu_);
  auto v = std::make_shared<Version>();
  v->levels.resize(opt_.num_levels);
  uint64_t log_number = 0;
  std::unordered_set<uint64_t> live_tables;

  const std::string manifest_path = dir_ + "/MANIFEST";
  if (FileExists(manifest_path)) {
    std::string data;
    s = ReadFileToString(manifest_path, &data);
    if (!s.ok()) return s;
    if (data.size() < 24 + 4 ||
        crc32c::Value(data.data(), data.size() - 4) != DecodeFixed32(data.data() + data.size() - 4) ||
        DecodeFixed64(data.data()) != kManifestMagic) {
      return Status::Corruption("MANIFEST checksum mismatch");
    }
    std::string_view in(data.data() + 8, data.size() - 12);
    next_file_number_ = DecodeFixed64(in.data());
    log_number = DecodeFixed64(in.data() + 8);
    in.remove_prefix(16);
    while (!in.empty()) {
      if (in.size() < 28) return Status::Corruption("MANIFEST truncated");
      const uint32_t level = DecodeFixed32(in.data());
      auto f = std::make_shared<FileMeta>();
      f->number = DecodeFixed64(in.data() + 4);
      f->file_size = DecodeFixed64(in.data() + 12);
      f->num_entries = DecodeFixed64(in.data() + 20);
      in.remove_prefix(28);
      std::string_view smallest, largest;
      if (!GetLengthPrefixed(&in, &smallest) || !GetLengthPrefixed(&in, &largest) ||
          level >= static_cast<uint32_t>(opt_.num_levels)) {
        return Status::Corruption("MANIFEST entry malformed");
      }
      f->smallest.assign(smallest);
      f->largest.assign(largest);
      s = TableReader::Open(TableFileName(dir_, f->number), &f->table);
      if (!s.ok()) return s;
      live_tables.insert(f->number);
      v->levels[level].push_back(std::move(f));
    }
  }
  std::sort(v->levels[0].begin(), v->levels[0].end(),
            [](const FilePtr& a, const FilePtr& b) { return a->number > b->number; });
  for (int level = 1; level < opt_.num_levels; ++level) SortLevel(&v->levels[level]);

  // Classify files: replay logs that were not yet flushed, delete everything else
  // the manifest does not reference (leftovers of a crash mid-flush or mid-compaction).
  std::vector<std::string> names;
  s = ListDir(dir_, &names);
  if (!s.ok()) return s;
  std::vector<uint64_t> logs;
  for (const std::string& name : names) {
    uint64_t number = 0;
    const FileKind kind = ParseFileName(name, &number);
    if (kind == FileKind::kOther) {
      if (name == "MANIFEST.tmp") RemoveFile(dir_ + "/" + name);
      continue;
    }
    next_file_number_ = std::max<uint64_t>(next_file_number_, number + 1);
    if (kind == FileKind::kTable && live_tables.count(number) == 0) {
      RemoveFile(dir_ + "/" + name);
    } else if (kind == FileKind::kLog) {
      if (number >= log_number) {
        logs.push_back(number);
      } else {
        RemoveFile(dir_ + "/" + name);
      }
    }
  }
  std::sort(logs.begin(), logs.end());

  auto recovered = std::make_shared<MemTable>(0);
  for (uint64_t n : logs) {
    uint64_t dropped = 0;
    s = ReadLog(
        LogFileName(dir_, n),
        [&](std::string_view rec) {
          return ApplyBatch(rec, recovered.get(), &stats_.recovered_records);
        },
        &dropped);
    if (!s.ok()) return s;
    stats_.wal_bytes_dropped += dropped;
  }
  if (recovered->Count() > 0) {
    auto it = recovered->NewIterator();
    it->SeekToFirst();
    FilePtr f;
    s = WriteTable(it.get(), &f);
    if (!s.ok()) return s;
    v->levels[0].insert(v->levels[0].begin(), f);
    stats_.flush_bytes_written += f->file_size;
  }

  const uint64_t new_log = next_file_number_++;
  s = LogWriter::Open(LogFileName(dir_, new_log), &log_);
  if (!s.ok()) return s;
  mem_ = std::make_shared<MemTable>(new_log);
  current_ = v;
  s = WriteManifest(*v, new_log);
  if (!s.ok()) return s;
  for (uint64_t n : logs) RemoveFile(LogFileName(dir_, n));

  bg_thread_ = std::thread(&DBImpl::BackgroundLoop, this);
  return Status::OK();
}

Status DBImpl::WriteManifest(const Version& v, uint64_t log_number) {
  std::string rep;
  PutFixed64(&rep, kManifestMagic);
  PutFixed64(&rep, next_file_number_.load());
  PutFixed64(&rep, log_number);
  for (size_t level = 0; level < v.levels.size(); ++level) {
    for (const FilePtr& f : v.levels[level]) {
      PutFixed32(&rep, static_cast<uint32_t>(level));
      PutFixed64(&rep, f->number);
      PutFixed64(&rep, f->file_size);
      PutFixed64(&rep, f->num_entries);
      PutLengthPrefixed(&rep, f->smallest);
      PutLengthPrefixed(&rep, f->largest);
    }
  }
  PutFixed32(&rep, crc32c::Value(rep.data(), rep.size()));
  return AtomicWriteFile(dir_, dir_ + "/MANIFEST", rep);
}

// ---------------------------------------------------------------- writes

Status DBImpl::Write(const WriteOptions& options, const WriteBatch& batch) {
  if (batch.count_ == 0) return Status::OK();
  std::unique_lock<std::mutex> lock(mu_);
  Status s = MakeRoomForWrite(lock, false);
  if (!s.ok()) return s;
  s = log_->AddRecord(batch.rep_, options.sync);
  if (!s.ok()) return s;
  uint64_t applied = 0;
  s = ApplyBatch(batch.rep_, mem_.get(), &applied);
  stats_.user_bytes_written += batch.rep_.size();
  stats_.wal_bytes_written += batch.rep_.size() + 8;
  return s;
}

Status DBImpl::MakeRoomForWrite(std::unique_lock<std::mutex>& lock, bool force) {
  bool stalled = false;
  while (true) {
    if (!bg_error_.ok()) return bg_error_;
    if (force ? mem_->Count() == 0 : mem_->ApproximateMemoryUsage() < opt_.write_buffer_size) {
      return Status::OK();
    }
    if (imm_.size() >= opt_.max_immutable_memtables ||
        current_->levels[0].size() >= static_cast<size_t>(opt_.l0_stop_writes_trigger)) {
      // Backpressure: the background thread is behind, wait for it to catch up.
      if (!stalled) ++stats_.write_stalls;
      stalled = true;
      bg_cv_.notify_one();
      done_cv_.wait(lock);
      continue;
    }
    const uint64_t number = next_file_number_++;
    std::unique_ptr<LogWriter> log;
    Status s = LogWriter::Open(LogFileName(dir_, number), &log);
    if (!s.ok()) return s;
    log_ = std::move(log);
    imm_.insert(imm_.begin(), mem_);
    mem_ = std::make_shared<MemTable>(number);
    bg_cv_.notify_one();
    return Status::OK();
  }
}

// ---------------------------------------------------------------- reads

Status DBImpl::Get(std::string_view key, std::string* value) {
  std::shared_ptr<MemTable> mem;
  std::vector<std::shared_ptr<MemTable>> imm;
  std::shared_ptr<const Version> v;
  {
    std::lock_guard<std::mutex> l(mu_);
    mem = mem_;
    imm = imm_;
    v = current_;
  }

  ValueType type = ValueType::kValue;
  auto result = [&]() { return type == ValueType::kValue ? Status::OK() : Status::NotFound(); };
  if (mem->Get(key, value, &type)) return result();
  for (const auto& m : imm) {
    if (m->Get(key, value, &type)) return result();
  }

  auto probe = [&](const FileMeta& f, bool* found) -> Status {
    table_probes_.fetch_add(1, std::memory_order_relaxed);
    TableReader::Lookup lookup;
    Status s = f.table->Get(key, value, &type, &lookup);
    if (lookup == TableReader::Lookup::kFilteredOut) {
      bloom_negatives_.fetch_add(1, std::memory_order_relaxed);
    }
    *found = lookup == TableReader::Lookup::kFound;
    return s;
  };

  for (size_t level = 0; level < v->levels.size(); ++level) {
    const auto& files = v->levels[level];
    if (level == 0) {
      for (const FilePtr& f : files) {  // newest first; ranges may overlap
        if (!Overlaps(*f, key, key)) continue;
        bool found = false;
        Status s = probe(*f, &found);
        if (!s.ok()) return s;
        if (found) return result();
      }
      continue;
    }
    auto it = std::lower_bound(files.begin(), files.end(), key,
                               [](const FilePtr& f, std::string_view k) {
                                 return std::string_view(f->largest) < k;
                               });
    if (it == files.end() || std::string_view((*it)->smallest) > key) continue;
    bool found = false;
    Status s = probe(**it, &found);
    if (!s.ok()) return s;
    if (found) return result();
  }
  return Status::NotFound();
}

Status DBImpl::Scan(std::string_view start, std::string_view end, size_t limit,
                    std::vector<std::pair<std::string, std::string>>* out) {
  out->clear();
  std::shared_ptr<MemTable> mem;
  std::vector<std::shared_ptr<MemTable>> imm;
  std::shared_ptr<const Version> v;
  {
    std::lock_guard<std::mutex> l(mu_);
    mem = mem_;
    imm = imm_;
    v = current_;
  }

  // Children in recency order so the merge returns the newest version of each key.
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(mem->NewIterator());
  for (const auto& m : imm) children.push_back(m->NewIterator());
  for (const FilePtr& f : v->levels[0]) children.push_back(f->table->NewIterator());
  for (size_t level = 1; level < v->levels.size(); ++level) {
    if (!v->levels[level].empty()) {
      children.push_back(std::make_unique<LevelIterator>(v->levels[level]));
    }
  }

  auto it = NewMergingIterator(std::move(children));
  for (it->Seek(start); it->Valid() && out->size() < limit; it->Next()) {
    if (!end.empty() && it->key() >= end) break;
    if (it->type() == ValueType::kDeletion) continue;
    out->emplace_back(std::string(it->key()), std::string(it->value()));
  }
  return it->status();
}

// ---------------------------------------------------------------- background work

void DBImpl::BackgroundLoop() {
  std::unique_lock<std::mutex> lock(mu_);
  while (!shutting_down_) {
    Compaction c;
    Status s;
    if (!bg_error_.ok()) {
      bg_cv_.wait(lock);
      continue;
    }
    if (!imm_.empty()) {
      bg_busy_ = true;
      s = FlushOldestImmutable(lock);
    } else if (PickCompaction(&c)) {
      bg_busy_ = true;
      s = RunCompaction(lock, c);
    } else {
      done_cv_.notify_all();
      bg_cv_.wait(lock);
      continue;
    }
    bg_busy_ = false;
    if (!s.ok()) bg_error_ = s;
    done_cv_.notify_all();
  }
  done_cv_.notify_all();
}

uint64_t DBImpl::OldestLiveLog() const {
  return imm_.empty() ? mem_->log_number() : imm_.back()->log_number();
}

Status DBImpl::FlushOldestImmutable(std::unique_lock<std::mutex>& lock) {
  std::shared_ptr<MemTable> m = imm_.back();
  lock.unlock();
  FilePtr f;
  Status s;
  {
    auto it = m->NewIterator();
    it->SeekToFirst();
    if (it->Valid()) s = WriteTable(it.get(), &f);
  }
  lock.lock();
  if (!s.ok()) return s;

  auto v = std::make_shared<Version>(*current_);
  if (f) v->levels[0].insert(v->levels[0].begin(), f);
  // Logs older than the next-oldest memtable are no longer needed after this flush.
  const uint64_t keep_log =
      imm_.size() > 1 ? imm_[imm_.size() - 2]->log_number() : mem_->log_number();
  s = WriteManifest(*v, keep_log);
  if (!s.ok()) return s;  // the unreferenced table is removed at next Recover()
  current_ = v;
  imm_.pop_back();
  RemoveFile(LogFileName(dir_, m->log_number()));
  ++stats_.flushes;
  if (f) stats_.flush_bytes_written += f->file_size;
  return Status::OK();
}

Status DBImpl::FinishTable(uint64_t number, TableBuilder* b, FilePtr* out) {
  Status s = b->Finish();
  if (!s.ok()) {
    b->Abandon();
    return s;
  }
  auto f = std::make_shared<FileMeta>();
  f->number = number;
  f->file_size = b->FileSize();
  f->num_entries = b->NumEntries();
  f->smallest = b->smallest();
  f->largest = b->largest();
  s = TableReader::Open(TableFileName(dir_, number), &f->table);
  if (!s.ok()) return s;
  *out = std::move(f);
  return Status::OK();
}

Status DBImpl::WriteTable(Iterator* it, FilePtr* out) {
  const uint64_t number = next_file_number_++;
  TableBuilder b(opt_, TableFileName(dir_, number));
  Status s = b.Open();
  if (!s.ok()) return s;
  for (; it->Valid(); it->Next()) b.Add(it->key(), it->value(), it->type());
  s = it->status();
  if (!s.ok()) {
    b.Abandon();
    return s;
  }
  return FinishTable(number, &b, out);
}

uint64_t DBImpl::MaxBytesForLevel(int level) const {
  uint64_t bytes = opt_.max_bytes_for_level_base;
  for (int i = 1; i < level; ++i) bytes *= 10;
  return bytes;
}

double DBImpl::LevelScore(const Version& v, int level) const {
  if (level == 0) {
    return static_cast<double>(v.levels[0].size()) / opt_.l0_compaction_trigger;
  }
  uint64_t bytes = 0;
  for (const FilePtr& f : v.levels[level]) bytes += f->file_size;
  return static_cast<double>(bytes) / static_cast<double>(MaxBytesForLevel(level));
}

bool DBImpl::NeedsCompaction() const {
  for (int level = 0; level + 1 < opt_.num_levels; ++level) {
    if (LevelScore(*current_, level) >= 1.0) return true;
  }
  return false;
}

bool DBImpl::PickCompaction(Compaction* c) {
  const Version& v = *current_;
  int best = -1;
  double best_score = 1.0;
  for (int level = 0; level + 1 < opt_.num_levels; ++level) {
    const double score = LevelScore(v, level);
    if (score >= best_score) {
      best = level;
      best_score = score;
    }
  }
  if (best < 0) return false;

  c->level = best;
  if (best == 0) {
    c->inputs[0] = v.levels[0];
  } else {
    // Round-robin through the key space so every file eventually gets compacted.
    const auto& files = v.levels[best];
    FilePtr pick = files.front();
    for (const FilePtr& f : files) {
      if (compact_pointer_[best].empty() || f->smallest > compact_pointer_[best]) {
        pick = f;
        break;
      }
    }
    c->inputs[0] = {pick};
    compact_pointer_[best] = pick->largest;
  }

  std::string smallest = c->inputs[0].front()->smallest;
  std::string largest = c->inputs[0].front()->largest;
  for (const FilePtr& f : c->inputs[0]) {
    smallest = std::min(smallest, f->smallest);
    largest = std::max(largest, f->largest);
  }
  for (const FilePtr& f : v.levels[best + 1]) {
    if (Overlaps(*f, smallest, largest)) c->inputs[1].push_back(f);
  }
  c->bottommost = true;
  for (int level = best + 2; level < opt_.num_levels && c->bottommost; ++level) {
    for (const FilePtr& f : v.levels[level]) {
      if (Overlaps(*f, smallest, largest)) {
        c->bottommost = false;
        break;
      }
    }
  }
  return true;
}

Status DBImpl::RunCompaction(std::unique_lock<std::mutex>& lock, const Compaction& c) {
  const int out_level = c.level + 1;
  auto remove_inputs = [&](Version* v) {
    std::unordered_set<uint64_t> gone;
    for (const auto& in : c.inputs) {
      for (const FilePtr& f : in) gone.insert(f->number);
    }
    for (int level : {c.level, out_level}) {
      auto& files = v->levels[level];
      files.erase(std::remove_if(files.begin(), files.end(),
                                 [&](const FilePtr& f) { return gone.count(f->number) > 0; }),
                  files.end());
    }
  };

  // Trivial move: a single file with nothing to merge against is relinked one level down.
  if (c.level > 0 && c.inputs[0].size() == 1 && c.inputs[1].empty()) {
    auto v = std::make_shared<Version>(*current_);
    remove_inputs(v.get());
    v->levels[out_level].push_back(c.inputs[0].front());
    SortLevel(&v->levels[out_level]);
    Status s = WriteManifest(*v, OldestLiveLog());
    if (!s.ok()) return s;
    current_ = v;
    ++stats_.trivial_moves;
    return Status::OK();
  }

  uint64_t bytes_read = 0;
  std::vector<std::unique_ptr<Iterator>> children;
  for (const FilePtr& f : c.inputs[0]) {  // L0 inputs are already newest first
    children.push_back(f->table->NewIterator());
    bytes_read += f->file_size;
  }
  if (!c.inputs[1].empty()) children.push_back(std::make_unique<LevelIterator>(c.inputs[1]));
  for (const FilePtr& f : c.inputs[1]) bytes_read += f->file_size;

  // Merge without holding the lock. Only this thread changes current_, so the
  // input files cannot disappear underneath us.
  lock.unlock();
  std::vector<FilePtr> outputs;
  std::unique_ptr<TableBuilder> builder;
  uint64_t number = 0;
  uint64_t dropped = 0;
  Status s;
  auto it = NewMergingIterator(std::move(children));
  for (it->SeekToFirst(); s.ok() && it->Valid(); it->Next()) {
    if (it->type() == ValueType::kDeletion && c.bottommost) {
      ++dropped;
      continue;
    }
    if (!builder) {
      number = next_file_number_++;
      builder = std::make_unique<TableBuilder>(opt_, TableFileName(dir_, number));
      s = builder->Open();
      if (!s.ok()) break;
    }
    builder->Add(it->key(), it->value(), it->type());
    if (builder->FileSize() >= opt_.target_file_size) {
      FilePtr f;
      s = FinishTable(number, builder.get(), &f);
      if (s.ok()) outputs.push_back(std::move(f));
      builder.reset();
    }
  }
  if (s.ok()) s = it->status();
  if (s.ok() && builder) {
    FilePtr f;
    s = FinishTable(number, builder.get(), &f);
    if (s.ok()) outputs.push_back(std::move(f));
    builder.reset();
  }
  if (!s.ok()) {
    if (builder) builder->Abandon();
    for (const FilePtr& f : outputs) RemoveFile(TableFileName(dir_, f->number));
    lock.lock();
    return s;
  }
  lock.lock();

  auto v = std::make_shared<Version>(*current_);
  remove_inputs(v.get());
  uint64_t bytes_written = 0;
  for (const FilePtr& f : outputs) {
    v->levels[out_level].push_back(f);
    bytes_written += f->file_size;
  }
  SortLevel(&v->levels[out_level]);
  s = WriteManifest(*v, OldestLiveLog());
  if (!s.ok()) {
    for (const FilePtr& f : outputs) RemoveFile(TableFileName(dir_, f->number));
    return s;
  }
  current_ = v;
  // Readers still holding the old Version keep their mappings; unlinking is safe.
  for (const auto& in : c.inputs) {
    for (const FilePtr& f : in) RemoveFile(TableFileName(dir_, f->number));
  }
  ++stats_.compactions;
  stats_.compaction_bytes_read += bytes_read;
  stats_.compaction_bytes_written += bytes_written;
  stats_.tombstones_dropped += dropped;
  return Status::OK();
}

Status DBImpl::Flush() {
  std::unique_lock<std::mutex> lock(mu_);
  Status s = MakeRoomForWrite(lock, true);
  if (!s.ok()) return s;
  while (!imm_.empty() && bg_error_.ok()) {
    bg_cv_.notify_one();
    done_cv_.wait(lock);
  }
  return bg_error_;
}

Status DBImpl::WaitForCompactions() {
  std::unique_lock<std::mutex> lock(mu_);
  while (bg_error_.ok() && (!imm_.empty() || bg_busy_ || NeedsCompaction())) {
    bg_cv_.notify_one();
    done_cv_.wait(lock);
  }
  return bg_error_;
}

DBStats DBImpl::GetStats() {
  std::lock_guard<std::mutex> l(mu_);
  DBStats s = stats_;
  s.table_probes = table_probes_.load();
  s.bloom_negatives = bloom_negatives_.load();
  for (const auto& files : current_->levels) {
    LevelStats ls;
    ls.files = static_cast<int>(files.size());
    for (const FilePtr& f : files) ls.bytes += f->file_size;
    s.levels.push_back(ls);
  }
  return s;
}

}  // namespace aerokv
