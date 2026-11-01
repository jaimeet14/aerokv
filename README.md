# AeroKV

An embedded key-value storage engine in C++17 built on a **log-structured merge tree**, the design behind LevelDB and RocksDB. It provides durable writes through a write-ahead log, crash recovery, bloom-filtered point lookups, range scans and leveled compaction running on a background thread.

```cpp
#include "aerokv/db.h"

std::unique_ptr<aerokv::DB> db;
aerokv::DB::Open(aerokv::Options(), "/tmp/mydb", &db);
db->Put({}, "user:42", "jaimeet");
std::string v;
db->Get("user:42", &v);                    // v == "jaimeet"
db->Delete({}, "user:42");

aerokv::WriteBatch batch;                  // applied atomically, even across a crash
batch.Put("a", "1");
batch.Delete("b");
aerokv::WriteOptions durable;
durable.sync = true;                       // fdatasync before returning
db->Write(durable, batch);

std::vector<std::pair<std::string, std::string>> rows;
db->Scan("user:", "user;", 100, &rows);    // range scan, newest version of each key
```

## Architecture

```
 Put/Delete ──► WAL (append, CRC32C per record) ──► MemTable (skiplist, 4 MB)
                                                       │ full → frozen, new WAL
                                                       ▼
                                          background thread: flush
                                                       ▼
 L0   [sst][sst][sst][sst]        newest first, ranges may overlap
          │ 4 files → merge with overlapping L1
 L1   [sst|sst|sst]               10 MB, sorted, non-overlapping
          │ over budget → pick one file round-robin, merge into L2
 L2   [sst|sst|sst|sst|sst|...]   100 MB
 ...                              each level 10x the previous

 Get:  MemTable → frozen MemTables → L0 (newest first) → one file per level (binary search)
       each SSTable: key-range check → bloom filter → index binary search → one 4 KB block
 Scan: k-way merge (min-heap) over MemTables, L0 files and one lazy iterator per level
```

| Component | File | Notes |
|---|---|---|
| Write-ahead log | `src/wal.cc` | One `write(2)` per record so acknowledged writes survive a process crash; `sync=true` adds `fdatasync` for power loss. Replay stops cleanly at a torn or corrupt tail. |
| MemTable | `src/memtable.cc` | Skiplist (p = 1/4, max height 12) under a reader-writer lock; frozen memtables are read without contention. |
| SSTable | `src/sstable.cc` | 4 KB data blocks with prefix-compressed keys, a block index, a bloom filter and a footer. Every block, the index and the filter carry a CRC32C. Files are `mmap`ed; block checksums are verified lazily on first read. |
| Bloom filter | `src/bloom.cc` | 10 bits/key, 7 probes derived from one 64-bit hash by double hashing (~0.8% false positives). |
| CRC32C | `src/crc32c.cc` | SSE4.2 `crc32` instruction when the CPU has it, table-driven fallback otherwise. |
| Iterators | `src/iterator.cc` | Min-heap k-way merge; on equal keys the newest source wins and shadowed versions are skipped. |
| Engine | `src/db.cc` | Copy-on-write `Version` snapshots, leveled compaction with trivial moves and tombstone GC, write backpressure, atomic MANIFEST, recovery. |

### Design decisions

- **Snapshot isolation for readers via `shared_ptr<const Version>`.** A read grabs pointers to the memtables and the current file set under the mutex and then runs lock-free. Compaction installs a new `Version` and unlinks the input files. A reader still holding the old version keeps working, because an unlinked file stays alive until its last mapping is unmapped.
- **One background thread, single writer of `current_`.** Flushes take priority over compaction. Because no one else changes the file set, compaction merges without holding the lock and takes it only briefly to install its result.
- **Crash safety comes from ordering.** The new SSTable is fsynced, then the MANIFEST is rewritten atomically (tmp file, fsync, rename, fsync of the directory), and only then is the old WAL deleted. A crash at any point leaves either the WAL or the table referenced by the MANIFEST, never neither. On open, unreferenced `.sst` files left by an interrupted flush or compaction are deleted.
- **Backpressure instead of unbounded memory.** Writes stall when two frozen memtables are waiting or L0 reaches 12 files.
- **Tombstones are dropped only at the bottommost level.** A delete is dropped only when no deeper level overlaps its key range, so a deleted value can never resurface.

## Benchmarks

`db_bench`, 1,000,000 keys (16 B) with 100 B values, 200,000 reads, against **SQLite 3** (WAL mode, one transaction per write, `synchronous=OFF` and `mmap_size=1GB`, so durability and read paths match AeroKV's defaults). Measured on a 2-vCPU Intel Xeon @ 2.1 GHz cloud VM. The numbers below are from one run; the relative results held across repeated runs.

| Workload | AeroKV ops/s | p99 | SQLite ops/s | p99 | |
|---|---:|---:|---:|---:|---|
| fillseq | 587,089 | 3.96 µs | 144,665 | 28.00 µs | **4.1×** |
| fillrandom | 285,725 | 7.26 µs | 63,159 | 54.55 µs | **4.5×** |
| readrandom | 248,126 | 10.96 µs | 285,569 | 8.93 µs | 0.87× |
| readmissing | 604,022 | 5.43 µs | 254,849 | 11.12 µs | **2.4×** |
| scan100 | 21,366 | 92.29 µs | 38,797 | 57.97 µs | 0.55× |

- **Random writes are 4.5× faster with a 7.5× lower p99.** Writes go to an append-only log plus an in-memory skiplist, never to an in-place B-tree page. SQLite's fillrandom p99.9 is 2.4 ms; AeroKV's is 47 µs.
- **Bloom filters rejected 99.1% of SSTable probes** for keys that fall inside a table's range but don't exist. That makes negative lookups 2.4× faster than SQLite.
- **Write amplification is 4.7×** (SSTable bytes written per user byte) across 67 flushes and 48 compactions, 7 of them trivial moves.
- **Where SQLite wins.** A random hit can touch several levels where SQLite walks one B-tree, so AeroKV's reads are about 13% slower. Range scans are about 1.8× slower, because every scan merges 5+ sorted sources. This is the classic LSM trade-off.

Reproduce with:

```sh
./build/db_bench --num=1000000 --reads=200000
```

## Testing

There are 26 GoogleTest cases. The CI matrix runs them under GCC and Clang, **AddressSanitizer + UBSan** and **ThreadSanitizer**.

- **Model-based test:** 90,000 random puts and deletes with flushes, multi-level compactions and three reopens, checked key by key and by full scan against a `std::map`.
- **Crash test:** a forked writer is killed with `SIGKILL` mid-stream. After recovery, every acknowledged write must be present and the surviving keys must form a gap-free prefix.
- **Torn WAL tail:** a `WriteBatch` truncated mid-record is discarded as a whole, so batches stay atomic across a crash.
- **Corruption:** a flipped byte in a data block yields `Status::Corruption`. Neither `Get` nor `Scan` returns bad data.
- **Concurrency:** 3 reader threads run `Get` and `Scan` while a writer forces flushes and compactions. Every value read must be consistent.
- **Other cases:** orphan-file cleanup, single-process directory locking, bloom false-positive rate, CRC32C against the RFC 3720 vector, and hardware and software CRC agreement.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/aerokv_tests
cmake -S . -B build-tsan -DAEROKV_SANITIZE=thread && cmake --build build-tsan && ./build-tsan/aerokv_tests
```

The build needs CMake ≥ 3.16 and a C++17 compiler. GoogleTest is fetched automatically, and SQLite 3 is optional, used only by `db_bench`.

## Limitations

- **Only the latest version of each key is kept.** There are no sequence numbers, so there are no point-in-time snapshots or MVCC.
- **No block compression and no block cache.** The OS page cache serves reads through `mmap`.
- **Writers are serialised.** There is no group commit, so concurrent writers each pay their own `write(2)`.
- **There is a single compaction thread.**

## License

MIT
