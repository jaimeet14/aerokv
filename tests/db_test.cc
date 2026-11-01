// End-to-end tests of the DB: correctness against a std::map model across
// flushes, compactions and reopens, crash recovery, and concurrent access.
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <map>
#include <random>
#include <thread>

#include "aerokv/db.h"
#include "file_util.h"
#include "gtest/gtest.h"
#include "test_util.h"

namespace aerokv {
namespace {

using test::Key;
using test::TempDir;

// Tiny buffers so small tests exercise flushes and multi-level compaction.
Options SmallOptions() {
  Options o;
  o.write_buffer_size = 64 << 10;
  o.block_size = 1024;
  o.target_file_size = 64 << 10;
  o.max_bytes_for_level_base = 256 << 10;
  return o;
}

std::unique_ptr<DB> OpenOrDie(const std::string& path, const Options& o = SmallOptions()) {
  std::unique_ptr<DB> db;
  Status s = DB::Open(o, path, &db);
  if (!s.ok()) ADD_FAILURE() << s.ToString();
  return db;
}

std::string GetOr(DB* db, const std::string& key, const std::string& missing = "<missing>") {
  std::string v;
  Status s = db->Get(key, &v);
  if (s.IsNotFound()) return missing;
  EXPECT_TRUE(s.ok()) << s.ToString();
  return v;
}

void ExpectMatchesModel(DB* db, const std::map<std::string, std::string>& model,
                        uint64_t key_space) {
  for (uint64_t i = 0; i < key_space; ++i) {
    auto it = model.find(Key(i));
    ASSERT_EQ(GetOr(db, Key(i)), it == model.end() ? "<missing>" : it->second) << Key(i);
  }
  std::vector<std::pair<std::string, std::string>> all;
  ASSERT_TRUE(db->Scan("", "", SIZE_MAX, &all).ok());
  ASSERT_EQ(all.size(), model.size());
  size_t i = 0;
  for (const auto& [k, v] : model) {
    ASSERT_EQ(all[i].first, k);
    ASSERT_EQ(all[i].second, v);
    ++i;
  }
}

TEST(DB, BasicOperations) {
  TempDir dir;
  auto db = OpenOrDie(dir.path());
  WriteOptions w;
  ASSERT_TRUE(db->Put(w, "apple", "red").ok());
  ASSERT_TRUE(db->Put(w, "banana", "yellow").ok());
  EXPECT_EQ(GetOr(db.get(), "apple"), "red");
  ASSERT_TRUE(db->Put(w, "apple", "green").ok());
  EXPECT_EQ(GetOr(db.get(), "apple"), "green");
  ASSERT_TRUE(db->Delete(w, "banana").ok());
  EXPECT_EQ(GetOr(db.get(), "banana"), "<missing>");
  EXPECT_EQ(GetOr(db.get(), "cherry"), "<missing>");
  ASSERT_TRUE(db->Put(w, "empty", "").ok());
  EXPECT_EQ(GetOr(db.get(), "empty"), "");
}

TEST(DB, DeleteShadowsOlderValueOnDisk) {
  TempDir dir;
  auto db = OpenOrDie(dir.path());
  ASSERT_TRUE(db->Put({}, "k", "v1").ok());
  ASSERT_TRUE(db->Flush().ok());  // v1 now lives in an SSTable
  ASSERT_TRUE(db->Delete({}, "k").ok());
  EXPECT_EQ(GetOr(db.get(), "k"), "<missing>");
  ASSERT_TRUE(db->Flush().ok());  // the tombstone is now on disk too
  EXPECT_EQ(GetOr(db.get(), "k"), "<missing>");
  std::vector<std::pair<std::string, std::string>> out;
  ASSERT_TRUE(db->Scan("", "", 10, &out).ok());
  EXPECT_TRUE(out.empty());
}

TEST(DB, ScanRangeLimitAndTombstones) {
  TempDir dir;
  auto db = OpenOrDie(dir.path());
  for (int i = 0; i < 1000; ++i) ASSERT_TRUE(db->Put({}, Key(i), "v" + std::to_string(i)).ok());
  ASSERT_TRUE(db->Flush().ok());
  for (int i = 0; i < 1000; i += 2) ASSERT_TRUE(db->Delete({}, Key(i)).ok());

  std::vector<std::pair<std::string, std::string>> out;
  ASSERT_TRUE(db->Scan(Key(100), Key(120), 1000, &out).ok());
  ASSERT_EQ(out.size(), 10u);  // odd keys 101..119
  EXPECT_EQ(out.front().first, Key(101));
  EXPECT_EQ(out.back().first, Key(119));

  ASSERT_TRUE(db->Scan(Key(500), "", 5, &out).ok());
  ASSERT_EQ(out.size(), 5u);
  EXPECT_EQ(out[4].first, Key(509));
}

TEST(DB, RandomizedAgainstModelWithCompactionAndReopen) {
  TempDir dir;
  std::map<std::string, std::string> model;
  std::mt19937_64 rng(2026);
  constexpr uint64_t kKeySpace = 5000;
  const std::string filler(80, 'x');

  for (int round = 0; round < 3; ++round) {
    auto db = OpenOrDie(dir.path());
    for (int i = 0; i < 30000; ++i) {
      const std::string k = Key(rng() % kKeySpace);
      if (rng() % 5 == 0) {
        ASSERT_TRUE(db->Delete({}, k).ok());
        model.erase(k);
      } else {
        const std::string v = std::to_string(rng()) + filler;
        ASSERT_TRUE(db->Put({}, k, v).ok());
        model[k] = v;
      }
    }
    ExpectMatchesModel(db.get(), model, kKeySpace);  // with live memtables and compaction
    ASSERT_TRUE(db->WaitForCompactions().ok());
    ExpectMatchesModel(db.get(), model, kKeySpace);

    const DBStats st = db->GetStats();
    EXPECT_GT(st.flushes, 0u);
    EXPECT_GT(st.compactions, 0u);
  }  // destructor closes; next round recovers from the WAL + MANIFEST

  auto db = OpenOrDie(dir.path());
  ExpectMatchesModel(db.get(), model, kKeySpace);
  const DBStats st = db->GetStats();
  int deep_files = 0;
  for (size_t l = 1; l < st.levels.size(); ++l) deep_files += st.levels[l].files;
  EXPECT_GT(deep_files, 0);
}

TEST(DB, BottommostCompactionDropsTombstones) {
  TempDir dir;
  auto db = OpenOrDie(dir.path());
  for (int i = 0; i < 2000; ++i) ASSERT_TRUE(db->Put({}, Key(i), std::string(50, 'v')).ok());
  for (int i = 0; i < 2000; ++i) ASSERT_TRUE(db->Delete({}, Key(i)).ok());
  ASSERT_TRUE(db->Flush().ok());
  ASSERT_TRUE(db->WaitForCompactions().ok());
  EXPECT_GT(db->GetStats().tombstones_dropped, 0u);
  std::vector<std::pair<std::string, std::string>> out;
  ASSERT_TRUE(db->Scan("", "", 10, &out).ok());
  EXPECT_TRUE(out.empty());
}

TEST(DB, RecoversUnflushedWritesFromWal) {
  TempDir dir;
  {
    Options o;  // 4 MB buffer: everything below stays in the memtable / WAL
    auto db = OpenOrDie(dir.path(), o);
    for (int i = 0; i < 1000; ++i) ASSERT_TRUE(db->Put({}, Key(i), "v" + std::to_string(i)).ok());
    ASSERT_TRUE(db->Delete({}, Key(7)).ok());
    EXPECT_EQ(db->GetStats().flushes, 0u);
  }
  auto db = OpenOrDie(dir.path());
  EXPECT_EQ(db->GetStats().recovered_records, 1001u);
  EXPECT_EQ(GetOr(db.get(), Key(999)), "v999");
  EXPECT_EQ(GetOr(db.get(), Key(7)), "<missing>");
}

TEST(DB, WriteBatchIsAtomicAcrossTornWalTail) {
  TempDir dir;
  {
    auto db = OpenOrDie(dir.path(), Options());
    ASSERT_TRUE(db->Put({}, "before", "1").ok());
    WriteBatch b;
    for (int i = 0; i < 100; ++i) b.Put(Key(i), "batched");
    ASSERT_TRUE(db->Write({}, b).ok());
  }
  // Tear the last record (the batch) as if the process died mid-write.
  std::vector<std::string> names;
  ASSERT_TRUE(ListDir(dir.path(), &names).ok());
  for (const auto& n : names) {
    uint64_t num;
    if (ParseFileName(n, &num) == FileKind::kLog) {
      const std::string p = dir.path() + "/" + n;
      std::string data;
      ASSERT_TRUE(ReadFileToString(p, &data).ok());
      if (!data.empty()) ASSERT_EQ(::truncate(p.c_str(), static_cast<off_t>(data.size() - 10)), 0);
    }
  }
  auto db = OpenOrDie(dir.path());
  EXPECT_EQ(GetOr(db.get(), "before"), "1");
  for (int i = 0; i < 100; ++i) EXPECT_EQ(GetOr(db.get(), Key(i)), "<missing>");
  EXPECT_GT(db->GetStats().wal_bytes_dropped, 0u);
}

// Kills a writer process with SIGKILL at an arbitrary point and checks that the
// recovered state is exactly a prefix of the acknowledged writes: nothing
// acknowledged is lost and no later write appears without an earlier one.
TEST(DB, SurvivesSigkillDuringWrites) {
  for (int trial = 0; trial < 3; ++trial) {
    TempDir dir;
    int pipefd[2];
    ASSERT_EQ(::pipe(pipefd), 0);
    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
      ::close(pipefd[0]);
      std::unique_ptr<DB> db;
      if (!DB::Open(SmallOptions(), dir.path(), &db).ok()) ::_exit(1);
      for (uint64_t i = 0;; ++i) {
        if (!db->Put({}, Key(i), "value-" + std::to_string(i)).ok()) ::_exit(2);
        if (i % 1000 == 999) {  // report acknowledged progress
          const uint64_t acked = i + 1;
          if (::write(pipefd[1], &acked, sizeof(acked)) != sizeof(acked)) ::_exit(3);
        }
      }
    }
    ::close(pipefd[1]);
    uint64_t acked = 0, msg = 0;
    while (acked < 20000 + 7000 * trial &&
           ::read(pipefd[0], &msg, sizeof(msg)) == static_cast<ssize_t>(sizeof(msg))) {
      acked = msg;
    }
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
    ::close(pipefd[0]);
    ASSERT_TRUE(WIFSIGNALED(status)) << "writer exited early with " << WEXITSTATUS(status);

    auto db = OpenOrDie(dir.path());
    uint64_t present = 0;
    while (GetOr(db.get(), Key(present)) != "<missing>") {
      ASSERT_EQ(GetOr(db.get(), Key(present)), "value-" + std::to_string(present));
      ++present;
    }
    EXPECT_GE(present, acked);  // every acknowledged write survived
    for (uint64_t i = present; i < present + 1000; ++i) {
      ASSERT_EQ(GetOr(db.get(), Key(i)), "<missing>") << "hole before " << Key(i);
    }
  }
}

TEST(DB, OrphanTablesAreDeletedOnOpen) {
  TempDir dir;
  { auto db = OpenOrDie(dir.path()); }
  const std::string orphan = TableFileName(dir.path(), 999);
  std::FILE* f = std::fopen(orphan.c_str(), "w");
  ASSERT_NE(f, nullptr);
  std::fputs("half-written compaction output", f);
  std::fclose(f);
  auto db = OpenOrDie(dir.path());
  EXPECT_FALSE(FileExists(orphan));
}

TEST(DB, SecondInstanceIsLockedOut) {
  TempDir dir;
  auto db = OpenOrDie(dir.path());
  std::unique_ptr<DB> second;
  EXPECT_TRUE(DB::Open(SmallOptions(), dir.path(), &second).IsIOError());
}

TEST(DB, ConcurrentReadersSeeConsistentValuesDuringCompaction) {
  TempDir dir;
  auto db = OpenOrDie(dir.path());
  constexpr int kKeys = 2000;
  for (int i = 0; i < kKeys; ++i) ASSERT_TRUE(db->Put({}, Key(i), Key(i) + "@0").ok());

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> reads{0};
  std::atomic<int> errors{0};
  std::vector<std::thread> readers;
  for (int t = 0; t < 3; ++t) {
    readers.emplace_back([&, t] {
      std::mt19937 rng(t);
      std::vector<std::pair<std::string, std::string>> out;
      while (!stop.load()) {
        const std::string k = Key(rng() % kKeys);
        std::string v;
        // Every value must belong to its key; it may be from any generation.
        if (!db->Get(k, &v).ok() || v.compare(0, k.size() + 1, k + "@") != 0) ++errors;
        if (!db->Scan(k, "", 20, &out).ok()) ++errors;
        for (const auto& [sk, sv] : out) {
          if (sv.compare(0, sk.size() + 1, sk + "@") != 0) ++errors;
        }
        reads.fetch_add(1);
      }
    });
  }
  for (int gen = 1; gen <= 15; ++gen) {
    for (int i = 0; i < kKeys; ++i) {
      ASSERT_TRUE(db->Put({}, Key(i), Key(i) + "@" + std::to_string(gen) + std::string(40, 'p')).ok());
    }
  }
  stop = true;
  for (auto& r : readers) r.join();
  EXPECT_EQ(errors.load(), 0);
  EXPECT_GT(reads.load(), 0u);
  EXPECT_GT(db->GetStats().compactions, 0u);
}

TEST(DB, SyncWritesAndStatsAccounting) {
  TempDir dir;
  auto db = OpenOrDie(dir.path());
  WriteOptions sync;
  sync.sync = true;
  for (int i = 0; i < 50; ++i) ASSERT_TRUE(db->Put(sync, Key(i), "durable").ok());
  const DBStats st = db->GetStats();
  EXPECT_GT(st.user_bytes_written, 0u);
  EXPECT_GT(st.wal_bytes_written, st.user_bytes_written);
}

}  // namespace
}  // namespace aerokv
