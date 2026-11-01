// db_bench: throughput and latency of AeroKV, optionally side by side with SQLite.
//
//   db_bench [--num=1000000] [--reads=200000] [--value_size=100] [--engine=both|aerokv|sqlite]
//            [--dir=/tmp/aerokv_bench] [--sync]
//
// Keys are 16-byte zero-padded decimals; values are pseudo-random bytes. readmissing
// looks up keys that sort between existing ones, so every lookup lands inside
// some SSTable's key range and must be answered by its bloom filter or its data.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "aerokv/db.h"

#ifdef AEROKV_WITH_SQLITE
#include <sqlite3.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

struct Config {
  uint64_t num = 1000000;
  uint64_t reads = 200000;
  size_t value_size = 100;
  std::string engine = "both";
  std::string dir = "/tmp/aerokv_bench";
  bool sync = false;
};

std::string MakeKey(uint64_t i) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%016llu", static_cast<unsigned long long>(i % 10000000000000000ULL));
  return std::string(buf, 16);
}

class ValueGen {
 public:
  explicit ValueGen(size_t size) : size_(size) {
    std::mt19937 rng(301);
    data_.resize(1 << 20);
    for (char& c : data_) c = static_cast<char>('a' + rng() % 26);
  }
  std::string_view Next() {
    if (pos_ + size_ > data_.size()) pos_ = 0;
    std::string_view v(data_.data() + pos_, size_);
    pos_ += 7;
    return v;
  }

 private:
  size_t size_;
  size_t pos_ = 0;
  std::string data_;
};

// Engine-neutral interface so both stores run the identical workload.
class Engine {
 public:
  virtual ~Engine() = default;
  virtual bool Put(const std::string& k, std::string_view v) = 0;
  virtual bool Get(const std::string& k, std::string* v) = 0;  // false if missing
  virtual size_t Scan(const std::string& start, size_t n) = 0;
  virtual void Settle() {}  // let background work finish before read phases
  virtual void Report() {}
  // (SSTable probes, probes rejected by the bloom filter); zero for engines without one.
  virtual std::pair<uint64_t, uint64_t> BloomCounters() { return {0, 0}; }
};

class AeroEngine final : public Engine {
 public:
  AeroEngine(const std::string& path, bool sync) {
    wo_.sync = sync;
    aerokv::Status s = aerokv::DB::Open(aerokv::Options(), path, &db_);
    if (!s.ok()) {
      std::fprintf(stderr, "open: %s\n", s.ToString().c_str());
      std::exit(1);
    }
  }
  bool Put(const std::string& k, std::string_view v) override { return db_->Put(wo_, k, v).ok(); }
  bool Get(const std::string& k, std::string* v) override { return db_->Get(k, v).ok(); }
  size_t Scan(const std::string& start, size_t n) override {
    db_->Scan(start, "", n, &out_);
    return out_.size();
  }
  void Settle() override { db_->WaitForCompactions(); }
  void Report() override {
    const aerokv::DBStats st = db_->GetStats();
    std::printf("  levels:");
    for (size_t l = 0; l < st.levels.size(); ++l) {
      std::printf(" L%zu=%d files/%.1fMB", l, st.levels[l].files, st.levels[l].bytes / 1048576.0);
    }
    std::printf("\n  flushes=%llu compactions=%llu trivial_moves=%llu write_stalls=%llu\n",
                static_cast<unsigned long long>(st.flushes),
                static_cast<unsigned long long>(st.compactions),
                static_cast<unsigned long long>(st.trivial_moves),
                static_cast<unsigned long long>(st.write_stalls));
    std::printf("  write amplification (SSTable bytes / user bytes) = %.2f\n",
                st.WriteAmplification());
  }
  std::pair<uint64_t, uint64_t> BloomCounters() override {
    const aerokv::DBStats st = db_->GetStats();
    return {st.table_probes, st.bloom_negatives};
  }

 private:
  std::unique_ptr<aerokv::DB> db_;
  aerokv::WriteOptions wo_;
  std::vector<std::pair<std::string, std::string>> out_;
};

#ifdef AEROKV_WITH_SQLITE
class SqliteEngine final : public Engine {
 public:
  SqliteEngine(const std::string& path, bool sync) {
    std::filesystem::create_directories(path);
    if (sqlite3_open((path + "/bench.db").c_str(), &db_) != SQLITE_OK) std::exit(1);
    // WAL mode, one transaction per write. synchronous matches AeroKV's --sync
    // setting: OFF still write()s every commit but skips fsync, like AeroKV by default.
    Exec("PRAGMA journal_mode=WAL");
    Exec(sync ? "PRAGMA synchronous=FULL" : "PRAGMA synchronous=OFF");
    Exec("PRAGMA mmap_size=1073741824");  // same zero-copy reads AeroKV gets from mmap
    Exec("CREATE TABLE IF NOT EXISTS kv (k BLOB PRIMARY KEY, v BLOB) WITHOUT ROWID");
    Prepare("INSERT OR REPLACE INTO kv (k, v) VALUES (?, ?)", &put_);
    Prepare("SELECT v FROM kv WHERE k = ?", &get_);
    Prepare("SELECT k, v FROM kv WHERE k >= ? ORDER BY k LIMIT ?", &scan_);
  }
  ~SqliteEngine() override {
    sqlite3_finalize(put_);
    sqlite3_finalize(get_);
    sqlite3_finalize(scan_);
    sqlite3_close(db_);
  }
  bool Put(const std::string& k, std::string_view v) override {
    sqlite3_bind_blob(put_, 1, k.data(), static_cast<int>(k.size()), SQLITE_STATIC);
    sqlite3_bind_blob(put_, 2, v.data(), static_cast<int>(v.size()), SQLITE_STATIC);
    const bool ok = sqlite3_step(put_) == SQLITE_DONE;
    sqlite3_reset(put_);
    return ok;
  }
  bool Get(const std::string& k, std::string* v) override {
    sqlite3_bind_blob(get_, 1, k.data(), static_cast<int>(k.size()), SQLITE_STATIC);
    const bool found = sqlite3_step(get_) == SQLITE_ROW;
    if (found) {
      v->assign(static_cast<const char*>(sqlite3_column_blob(get_, 0)),
                static_cast<size_t>(sqlite3_column_bytes(get_, 0)));
    }
    sqlite3_reset(get_);
    return found;
  }
  size_t Scan(const std::string& start, size_t n) override {
    sqlite3_bind_blob(scan_, 1, start.data(), static_cast<int>(start.size()), SQLITE_STATIC);
    sqlite3_bind_int64(scan_, 2, static_cast<sqlite3_int64>(n));
    size_t rows = 0;
    while (sqlite3_step(scan_) == SQLITE_ROW) {
      row_.assign(static_cast<const char*>(sqlite3_column_blob(scan_, 1)),
                  static_cast<size_t>(sqlite3_column_bytes(scan_, 1)));
      ++rows;
    }
    sqlite3_reset(scan_);
    return rows;
  }

 private:
  void Exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
      std::fprintf(stderr, "sqlite: %s\n", err);
      std::exit(1);
    }
  }
  void Prepare(const char* sql, sqlite3_stmt** stmt) {
    if (sqlite3_prepare_v2(db_, sql, -1, stmt, nullptr) != SQLITE_OK) std::exit(1);
  }
  sqlite3* db_ = nullptr;
  sqlite3_stmt* put_ = nullptr;
  sqlite3_stmt* get_ = nullptr;
  sqlite3_stmt* scan_ = nullptr;
  std::string row_;
};
#endif

struct Result {
  std::string name;
  uint64_t ops = 0;
  double seconds = 0;
  double p50_us = 0, p99_us = 0, p999_us = 0;
  uint64_t found = 0;
};

Result Run(const std::string& name, uint64_t ops, const std::function<bool(uint64_t)>& op) {
  std::vector<float> lat;
  lat.reserve(ops);
  Result r;
  r.name = name;
  r.ops = ops;
  const auto start = Clock::now();
  for (uint64_t i = 0; i < ops; ++i) {
    const auto t0 = Clock::now();
    r.found += op(i);
    lat.push_back(std::chrono::duration<float, std::micro>(Clock::now() - t0).count());
  }
  r.seconds = std::chrono::duration<double>(Clock::now() - start).count();
  auto pct = [&](double p) {
    const size_t idx = std::min(lat.size() - 1, static_cast<size_t>(p * lat.size()));
    std::nth_element(lat.begin(), lat.begin() + static_cast<long>(idx), lat.end());
    return static_cast<double>(lat[idx]);
  };
  r.p50_us = pct(0.50);
  r.p99_us = pct(0.99);
  r.p999_us = pct(0.999);
  return r;
}

void Print(const Result& r) {
  std::printf("  %-12s %10.0f ops/s   p50 %7.2f us   p99 %8.2f us   p99.9 %8.2f us\n",
              r.name.c_str(), r.ops / r.seconds, r.p50_us, r.p99_us, r.p999_us);
}

std::vector<Result> RunSuite(const std::string& engine_name, const Config& cfg,
                             const std::function<std::unique_ptr<Engine>(const std::string&)>& open) {
  std::vector<Result> results;
  ValueGen values(cfg.value_size);
  std::mt19937_64 rng(42);
  const std::string base = cfg.dir + "/" + engine_name;

  std::printf("\n[%s] num=%llu value_size=%zu sync=%d\n", engine_name.c_str(),
              static_cast<unsigned long long>(cfg.num), cfg.value_size, cfg.sync);

  {  // sequential load into an empty store
    std::filesystem::remove_all(base + "_seq");
    auto e = open(base + "_seq");
    results.push_back(Run("fillseq", cfg.num, [&](uint64_t i) { return e->Put(MakeKey(i), values.Next()); }));
    Print(results.back());
  }

  std::filesystem::remove_all(base);
  auto e = open(base);
  std::vector<uint64_t> order(cfg.num);
  std::iota(order.begin(), order.end(), 0);
  std::shuffle(order.begin(), order.end(), rng);
  results.push_back(Run("fillrandom", cfg.num, [&](uint64_t i) { return e->Put(MakeKey(order[i]), values.Next()); }));
  Print(results.back());

  const auto settle_start = Clock::now();
  e->Settle();
  std::printf("  (background compaction settled in %.2fs)\n",
              std::chrono::duration<double>(Clock::now() - settle_start).count());

  std::string v;
  results.push_back(Run("readrandom", cfg.reads, [&](uint64_t) { return e->Get(MakeKey(rng() % cfg.num), &v); }));
  Print(results.back());
  if (results.back().found != cfg.reads) std::printf("  !! readrandom found only %llu\n", static_cast<unsigned long long>(results.back().found));

  const auto before = e->BloomCounters();
  results.push_back(Run("readmissing", cfg.reads, [&](uint64_t) { return !e->Get(MakeKey(rng() % cfg.num) + "#", &v); }));
  Print(results.back());
  const auto after = e->BloomCounters();
  if (after.first > before.first) {
    std::printf("  (bloom filters rejected %.2f%% of SSTable probes for missing keys)\n",
                100.0 * (after.second - before.second) / (after.first - before.first));
  }

  results.push_back(Run("scan100", cfg.reads / 10, [&](uint64_t) { return e->Scan(MakeKey(rng() % cfg.num), 100) > 0; }));
  Print(results.back());
  e->Report();
  return results;
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto val = [&](const char* flag) -> const char* {
      const size_t n = std::strlen(flag);
      return a.compare(0, n, flag) == 0 ? a.c_str() + n : nullptr;
    };
    if (const char* p = val("--num=")) cfg.num = std::strtoull(p, nullptr, 10);
    else if (const char* p = val("--reads=")) cfg.reads = std::strtoull(p, nullptr, 10);
    else if (const char* p = val("--value_size=")) cfg.value_size = std::strtoull(p, nullptr, 10);
    else if (const char* p = val("--engine=")) cfg.engine = p;
    else if (const char* p = val("--dir=")) cfg.dir = p;
    else if (a == "--sync") cfg.sync = true;
    else {
      std::fprintf(stderr, "unknown flag %s\n", argv[i]);
      return 1;
    }
  }
  std::filesystem::create_directories(cfg.dir);

  if (cfg.engine == "both" || cfg.engine == "aerokv") {
    RunSuite("aerokv", cfg, [&](const std::string& p) { return std::make_unique<AeroEngine>(p, cfg.sync); });
  }
#ifdef AEROKV_WITH_SQLITE
  if (cfg.engine == "both" || cfg.engine == "sqlite") {
    RunSuite("sqlite", cfg, [&](const std::string& p) { return std::make_unique<SqliteEngine>(p, cfg.sync); });
  }
#else
  if (cfg.engine == "sqlite") std::fprintf(stderr, "built without SQLite\n");
#endif
  return 0;
}
