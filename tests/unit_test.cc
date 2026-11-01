// Tests for the building blocks: encoding, checksums, bloom filter, memtable,
// merging iterator, WAL and SSTable format.
#include <fcntl.h>
#include <unistd.h>

#include <map>
#include <random>

#include "bloom.h"
#include "coding.h"
#include "crc32c.h"
#include "file_util.h"
#include "gtest/gtest.h"
#include "iterator.h"
#include "memtable.h"
#include "sstable.h"
#include "test_util.h"
#include "wal.h"

namespace aerokv {
namespace {

using test::Key;
using test::TempDir;

TEST(Coding, VarintRoundTrip) {
  std::string buf;
  const uint32_t values[] = {0, 1, 127, 128, 300, 16383, 16384, 1u << 28, 0xFFFFFFFFu};
  for (uint32_t v : values) PutVarint32(&buf, v);
  std::string_view in(buf);
  for (uint32_t v : values) {
    uint32_t got = 0;
    ASSERT_TRUE(GetVarint32(&in, &got));
    EXPECT_EQ(got, v);
  }
  EXPECT_TRUE(in.empty());
  std::string_view truncated("\x80", 1);
  uint32_t ignored;
  EXPECT_FALSE(GetVarint32(&truncated, &ignored));
}

TEST(Coding, FixedRoundTrip) {
  std::string buf;
  PutFixed32(&buf, 0xDEADBEEF);
  PutFixed64(&buf, 0x0123456789ABCDEFULL);
  EXPECT_EQ(DecodeFixed32(buf.data()), 0xDEADBEEFu);
  EXPECT_EQ(DecodeFixed64(buf.data() + 4), 0x0123456789ABCDEFULL);
}

TEST(Crc32c, KnownVectorAndHardwareMatchesSoftware) {
  EXPECT_EQ(crc32c::Value("123456789", 9), 0xE3069283u);  // RFC 3720 check value
  std::mt19937 rng(7);
  for (size_t len : {0, 1, 7, 8, 9, 63, 4096, 4099}) {
    std::string s(len, '\0');
    for (char& c : s) c = static_cast<char>(rng());
    EXPECT_EQ(crc32c::Value(s.data(), s.size()), crc32c::SoftwareValue(s.data(), s.size()));
  }
}

TEST(Bloom, NoFalseNegativesAndLowFalsePositiveRate) {
  std::vector<uint64_t> hashes;
  for (int i = 0; i < 10000; ++i) hashes.push_back(Hash64(Key(i)));
  const std::string filter = BuildBloomFilter(hashes, 10);
  for (int i = 0; i < 10000; ++i) ASSERT_TRUE(BloomMayContain(filter, Hash64(Key(i))));
  int false_positives = 0;
  for (int i = 10000; i < 110000; ++i) false_positives += BloomMayContain(filter, Hash64(Key(i)));
  const double rate = false_positives / 100000.0;
  EXPECT_LT(rate, 0.02);  // theory: ~0.8% at 10 bits/key
  EXPECT_TRUE(BloomMayContain("", 42));  // no filter means "maybe"
}

TEST(MemTable, PutOverwriteDeleteAndOrder) {
  auto m = std::make_shared<MemTable>(1);
  m->Add("b", "1", ValueType::kValue);
  m->Add("a", "2", ValueType::kValue);
  m->Add("c", "3", ValueType::kValue);
  m->Add("b", "updated", ValueType::kValue);
  m->Add("c", "", ValueType::kDeletion);

  std::string v;
  ValueType t;
  ASSERT_TRUE(m->Get("b", &v, &t));
  EXPECT_EQ(v, "updated");
  ASSERT_TRUE(m->Get("c", &v, &t));
  EXPECT_EQ(t, ValueType::kDeletion);
  EXPECT_FALSE(m->Get("d", &v, &t));
  EXPECT_EQ(m->Count(), 3u);

  auto it = m->NewIterator();
  std::string keys;
  for (it->SeekToFirst(); it->Valid(); it->Next()) keys += it->key();
  EXPECT_EQ(keys, "abc");
  it->Seek("bb");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "c");
}

TEST(MemTable, MatchesStdMapUnderRandomOps) {
  auto m = std::make_shared<MemTable>(1);
  std::map<std::string, std::string> model;
  std::mt19937 rng(42);
  for (int i = 0; i < 20000; ++i) {
    const std::string k = Key(rng() % 3000);
    const std::string v = std::to_string(rng());
    m->Add(k, v, ValueType::kValue);
    model[k] = v;
  }
  auto it = m->NewIterator();
  it->SeekToFirst();
  for (const auto& [k, v] : model) {
    ASSERT_TRUE(it->Valid());
    EXPECT_EQ(it->key(), k);
    EXPECT_EQ(it->value(), v);
    it->Next();
  }
  EXPECT_FALSE(it->Valid());
}

TEST(MergingIterator, NewestSourceWinsAndKeysAreDeduplicated) {
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(NewVectorIterator({{"b", "new", ValueType::kValue},
                                        {"d", "", ValueType::kDeletion}}));
  children.push_back(NewVectorIterator({{"a", "old", ValueType::kValue},
                                        {"b", "old", ValueType::kValue},
                                        {"d", "old", ValueType::kValue},
                                        {"e", "old", ValueType::kValue}}));
  auto it = NewMergingIterator(std::move(children));
  std::vector<std::string> seen;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    seen.push_back(std::string(it->key()) + "=" +
                   (it->type() == ValueType::kDeletion ? "DEL" : std::string(it->value())));
  }
  EXPECT_EQ(seen, (std::vector<std::string>{"a=old", "b=new", "d=DEL", "e=old"}));
}

TEST(Wal, RoundTripAndTornTail) {
  TempDir dir;
  const std::string path = dir.path() + "/000001.log";
  {
    std::unique_ptr<LogWriter> w;
    ASSERT_TRUE(LogWriter::Open(path, &w).ok());
    for (int i = 0; i < 100; ++i) ASSERT_TRUE(w->AddRecord("record" + std::to_string(i), false).ok());
  }
  // Simulate a crash in the middle of the last record.
  std::string data;
  ASSERT_TRUE(ReadFileToString(path, &data).ok());
  ASSERT_EQ(::truncate(path.c_str(), static_cast<off_t>(data.size() - 3)), 0);

  std::vector<std::string> got;
  uint64_t dropped = 0;
  ASSERT_TRUE(ReadLog(path, [&](std::string_view r) {
                got.emplace_back(r);
                return Status::OK();
              }, &dropped).ok());
  ASSERT_EQ(got.size(), 99u);
  EXPECT_EQ(got.back(), "record98");
  EXPECT_GT(dropped, 0u);
}

TEST(Wal, CorruptRecordStopsReplay) {
  TempDir dir;
  const std::string path = dir.path() + "/000001.log";
  {
    std::unique_ptr<LogWriter> w;
    ASSERT_TRUE(LogWriter::Open(path, &w).ok());
    for (int i = 0; i < 10; ++i) ASSERT_TRUE(w->AddRecord("payload-" + std::to_string(i), false).ok());
  }
  const int fd = ::open(path.c_str(), O_WRONLY);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(::pwrite(fd, "X", 1, 5 * 17 + 10), 1);  // flip a byte inside record 5's payload
  ::close(fd);

  size_t count = 0;
  uint64_t dropped = 0;
  ASSERT_TRUE(ReadLog(path, [&](std::string_view) { ++count; return Status::OK(); }, &dropped).ok());
  EXPECT_EQ(count, 5u);
}

class SSTableTest : public ::testing::Test {
 protected:
  void Build(int n, Options opt = Options()) {
    path_ = dir_.path() + "/000001.sst";
    TableBuilder b(opt, path_);
    ASSERT_TRUE(b.Open().ok());
    for (int i = 0; i < n; ++i) {
      const ValueType t = i % 10 == 0 ? ValueType::kDeletion : ValueType::kValue;
      b.Add(Key(i * 2), t == ValueType::kValue ? "value" + std::to_string(i) : "", t);
    }
    ASSERT_TRUE(b.Finish().ok());
    ASSERT_TRUE(TableReader::Open(path_, &table_).ok());
  }

  TempDir dir_;
  std::string path_;
  std::shared_ptr<TableReader> table_;
};

TEST_F(SSTableTest, PointLookups) {
  Build(10000);
  EXPECT_EQ(table_->num_entries(), 10000u);
  EXPECT_GT(table_->num_blocks(), 10u);
  std::string v;
  ValueType t;
  TableReader::Lookup r;
  for (int i = 0; i < 10000; ++i) {
    ASSERT_TRUE(table_->Get(Key(i * 2), &v, &t, &r).ok());
    ASSERT_EQ(r, TableReader::Lookup::kFound) << i;
    if (i % 10 == 0) {
      EXPECT_EQ(t, ValueType::kDeletion);
    } else {
      EXPECT_EQ(v, "value" + std::to_string(i));
    }
  }
  int filtered = 0;
  for (int i = 0; i < 10000; ++i) {
    ASSERT_TRUE(table_->Get(Key(i * 2 + 1), &v, &t, &r).ok());
    EXPECT_NE(r, TableReader::Lookup::kFound);
    filtered += r == TableReader::Lookup::kFilteredOut;
  }
  EXPECT_GT(filtered, 9700);  // the bloom filter answers ~99% of misses
}

TEST_F(SSTableTest, IteratorScanAndSeek) {
  Build(5000);
  auto it = table_->NewIterator();
  int n = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    ASSERT_EQ(it->key(), Key(n * 2));
    ++n;
  }
  EXPECT_EQ(n, 5000);
  it->Seek(Key(1001));  // between keys -> next even key
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), Key(1002));
  it->Seek(Key(99999));
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().ok());
}

TEST_F(SSTableTest, DetectsCorruptBlock) {
  Build(2000);
  table_.reset();
  const int fd = ::open(path_.c_str(), O_WRONLY);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(::pwrite(fd, "\xff\xff", 2, 100), 2);
  ::close(fd);
  ASSERT_TRUE(TableReader::Open(path_, &table_).ok());  // footer and index are intact

  std::string v;
  ValueType t;
  TableReader::Lookup r;
  EXPECT_TRUE(table_->Get(Key(2), &v, &t, &r).IsCorruption());
  auto it = table_->NewIterator();
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsCorruption());
}

TEST_F(SSTableTest, RejectsTruncatedFile) {
  Build(100);
  table_.reset();
  ASSERT_EQ(::truncate(path_.c_str(), 30), 0);
  EXPECT_TRUE(TableReader::Open(path_, &table_).IsCorruption());
}

TEST(FileNames, Parse) {
  uint64_t n = 0;
  EXPECT_EQ(ParseFileName("000123.sst", &n), FileKind::kTable);
  EXPECT_EQ(n, 123u);
  EXPECT_EQ(ParseFileName("000007.log", &n), FileKind::kLog);
  EXPECT_EQ(n, 7u);
  EXPECT_EQ(ParseFileName("MANIFEST", &n), FileKind::kOther);
  EXPECT_EQ(ParseFileName("12a.sst", &n), FileKind::kOther);
}

}  // namespace
}  // namespace aerokv
