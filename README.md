# CacheX

A small, Redis-inspired in-memory key–value cache server in C++17.

Built to be understood end to end, not to replace Redis. The point is to work
through the problems a real cache has to solve — O(1) lookup and eviction, key
expiry, concurrent access, and a network protocol — in a codebase small enough
that every design decision can be explained and defended.

See **[ARCHITECTURE.md](ARCHITECTURE.md)** for the design, the reasoning behind
it, and the full roadmap.

---

## Status

**Stage 8 of 11 — snapshot persistence.**

The cache can now survive a restart. `SAVE` writes every live entry to a snapshot
file, the server reloads it on startup, and an optional background thread saves on
an interval. TTLs are preserved as remaining time; entries that expired while the
file sat on disk are not restored. 205 tests pass, including under
AddressSanitizer, UndefinedBehaviorSanitizer and **ThreadSanitizer**, with zero
leaks.

⚠️ **This is a snapshot, not a database.** `fsync` is never called and there is no
write-ahead log, so everything written since the last save is lost on a crash.
[What it does not provide](ARCHITECTURE.md#107-what-this-is-not).

| | |
| --- | --- |
| ✅ Stage 1 | CMake build, Debug/Release, strict warnings, test framework |
| ✅ Stage 2 | Core cache (`unordered_map` + `std::list`), benchmark baseline |
| ✅ Stage 3 | Fixed capacity, O(1) LRU eviction, workload benchmarks |
| ✅ Stage 4 | Per-key TTL, lazy expiration, measured TTL cost |
| ✅ Stage 5 | TCP server, line protocol, CLI client, network benchmark |
| ✅ Stage 6 | Thread-per-connection, `SyncCache`, TSan clean, scaling benchmark |
| ✅ Stage 7 | Sharded cache, A/B/C/D × 1–16 client benchmark matrix |
| ✅ Stage 8 | Snapshot persistence, `SAVE`/`LOAD`, 205 tests |
| ⬜ Next | Stage 9 — benchmark harness II (sub-tick latency) |
| ⬜ Later | active expiry · event loop · optimisation |

## Try it

```console
$ ./build/release/bin/cachex_server 6379 &
CacheX 0.1.0 listening on 127.0.0.1:6379

$ ./build/release/bin/cachex_client localhost 6379
cachex> SET foo bar
+OK
cachex> GET foo
=bar
cachex> SET session abc 60
+OK
cachex> TTL session
:60
cachex> DELETE foo
:1
cachex> GET foo
_
cachex> QUIT
+BYE
```

It also speaks plain `netcat`, which is the point of a text protocol:

```console
$ printf 'SET foo bar\nGET foo\nQUIT\n' | nc 127.0.0.1 6379
+OK
=bar
+BYE
```

## API

```cpp
#include "cachex/cache.hpp"

cachex::Cache cache;                    // unbounded: never evicts
cachex::Cache lru(1000);                // bounded: evicts the least recently used

cache.set("user:1", "ada");             // insert or overwrite
cache.set("user:1", "ada lovelace");    // overwrite; size stays 1

if (auto value = cache.get("user:1")) { // std::optional -- nullopt on a miss
  std::cout << *value << "\n";          // a copy, not a reference into the cache
}

cache.contains("user:1");               // true  -- does NOT count as a "use"
cache.erase("user:1");                  // true  -- false if the key was absent
cache.size();                           // 0
lru.capacity();                         // optional<size_t>; nullopt if unbounded
lru.evictions();                        // lifetime count of evicted entries

// TTL
cache.set("session", "abc", 30s);       // expires 30 s from now
cache.set("session", "abc");            // a plain set CLEARS the TTL

cachex::TtlInfo info = cache.ttl("session");
switch (info.state) {
  case cachex::TtlState::Missing:    break;  // absent, or already expired
  case cachex::TtlState::Persistent: break;  // no expiry set
  case cachex::TtlState::Expiring:   break;  // info.remaining ms left
}
cache.expired_removals();               // lifetime count of expired entries reclaimed
```

All of `set`, `get`, `erase`, `contains`, `size`, **and eviction** are
**O(1) average** (O(n) worst case on hash collisions). Empty keys and empty
values are both legal, and an empty value is distinguishable from a missing key —
that is why `get` returns `std::optional`. Keys and values are length-delimited
`std::string`, so embedded null bytes are fine.

### Recency semantics

LRU only means something once "recently used" is pinned down:

| Operation | Counts as a use? | |
| --- | --- | --- |
| `get` hit | ✅ yes | The definition of LRU |
| `set` (insert or update) | ✅ yes | A write is at least as strong a signal as a read — matches Redis and memcached |
| `contains` | ❌ **no** | Peeking is not using: `EXISTS` must not keep dead data alive forever |
| `ttl` | ❌ **no** | A metadata query should not rescue a key from eviction |
| `erase` | ➖ n/a | Frees a slot, so the next insert need not evict |

### TTL semantics

| Case | Behaviour |
| --- | --- |
| `set(k, v)` on a key that had a TTL | TTL **cleared** — a set replaces the whole entry (Redis's `SET` default) |
| `set(k, v, ttl)` with **`ttl <= 0`** | Erases the key, stores nothing. *After `set(k, v, ttl)`, the key is visible iff `ttl > 0`* |
| `get` / `ttl` on an expired key | Reports missing **and reclaims** the entry |
| `contains` on an expired key | Reports `false` but does **not** reclaim — it is the one non-mutating peek |
| `erase` on an expired key | Returns `false` (nothing visible was removed) but reclaims it |
| `size()` with expired entries present | **Counts them.** It reports entries *resident*, not *visible* — the cost of lazy expiration |

Deadlines are stored as absolute `std::chrono::steady_clock` time points.
`steady_clock` is monotonic, so an NTP correction or a manual clock change cannot
resurrect an expired key or mass-expire the whole cache.

> ⚠️ **CacheX's TTL is intentionally simplified and does not behave like Redis.**
> Most importantly there is no active expiry sweep, so an expired key that is
> never touched again is never reclaimed — and it keeps occupying capacity, where
> it can evict a live entry. The full comparison is in
> [ARCHITECTURE.md §6.6](ARCHITECTURE.md#66-what-redis-does-differently).

### Thread safety

`Cache` is **single-threaded by design** — it pays for no locking. Two wrappers
make it shareable, with the same API as `Cache` itself:

```cpp
cachex::SyncCache   cache(1000);        // one mutex around everything
cachex::ShardedCache sharded(8, 1000);  // 8 independent shards, 1000 total
```

`ShardedCache` hashes the key to pick a shard and locks only that one. Shard count
is a pure tuning knob: behaviour is identical at any count, and a test asserts
that against 1, 4 and 16 shards.

Note that **`get()` takes an exclusive lock**, not a shared one. Under LRU a read
*is* a write: it splices the entry to the head of the recency list and may reclaim
an expired one. That is also why `std::shared_mutex` would buy little here — the
common operation in a cache is a reader that writes.

`SyncCache` makes each *operation* atomic, not sequences of them. A
get-then-set read-modify-write can still lose updates; that needs a compound
operation inside the lock, not a bigger mutex.

---

## Wire protocol

One command per line, one reply per line, terminated by `\n` (`\r\n` accepted).
Every reply starts with a one-byte type tag so a client can dispatch on a single
character.

| Request | Reply |
| --- | --- |
| `SET key value [ttl_seconds]` | `+OK` |
| `GET key` | `=value` or `_` (nil) |
| `DELETE key` (alias `DEL`) | `:1` or `:0` |
| `EXISTS key` | `:1` or `:0` |
| `TTL key` | `:seconds`, `+NOEXPIRE`, or `_` |
| `PING` | `+PONG` |
| `SAVE` | `:entries_written` — write a snapshot (requires a configured path) |
| `LOAD` | `:entries_loaded` — read the snapshot back |
| `QUIT` | `+BYE`, then the connection closes |
| anything invalid | `-ERR <reason>` — **connection stays open** |

> ⚠️ **Keys and values cannot contain whitespace or newlines**, because tokens are
> whitespace-delimited. The cache engine underneath stores arbitrary bytes; this
> is a limitation of the v1 *protocol*, and it is the clearest argument for
> length-prefixed framing (which is what RESP does).

Full specification, including limits, error cases, and the connection lifecycle:
**[docs/PROTOCOL.md](docs/PROTOCOL.md)**.

## Requirements

- A C++17 compiler — Apple Clang, GCC 9+, or MSVC 2019+
- CMake 3.16 or newer

No other dependencies. Nothing is fetched at configure time.

## Build

Out-of-source builds: all output goes into `build/`, which is gitignored.

**Debug** — no optimisation, full debug info. Use while developing.

```bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug -j
```

**Release** — `-O3 -DNDEBUG`. Use for anything you intend to measure.

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
```

### Build options

| Option | Default | Effect |
| --- | --- | --- |
| `CACHEX_BUILD_TESTS` | `ON` | Build the test suite |
| `CACHEX_BUILD_BENCHMARKS` | `OFF` | Build `cachex_bench` — opt-in, because it is only meaningful in a Release build |
| `CACHEX_WARNINGS_AS_ERRORS` | `OFF` | Turn warnings into errors — recommended while developing and in CI |

## Run

Three executables:

| Binary | What it is |
| --- | --- |
| `cachex_server [port] [capacity] [shards] [snapshot] [save-secs]` | The TCP server. Defaults: port 6379, capacity 0 (unbounded), 8 shards, persistence off. |
| `cachex_client [host] [port]` | Interactive CLI client. |
| `cachex` | In-process demo of the cache API — no networking, useful for seeing LRU and TTL directly. |

```bash
./build/release/bin/cachex_server 6379 1000   # bounded to 1000 entries
./build/release/bin/cachex_client localhost 6379
```

The demo binary:

```bash
./build/debug/bin/cachex
```

```
CacheX 0.1.0 -- in-process cache demo (no server yet)

[1] Basic operations (unbounded)
  get user:1  -> ada lovelace
  get user:42 -> (nil)
  erase user:2 -> true, again -> false, size -> 1

[2] LRU eviction (capacity 3)
  order (newest first): c b a
  get("a") -- reading it makes it the newest
  order (newest first): a c b
  set("d") -- at capacity, so the oldest ("b") is evicted
  order (newest first): d a c
  contains b -> false, contains a -> true, evictions -> 1
```

## Test

```bash
ctest --test-dir build/debug --output-on-failure
```

Or run the binary directly for per-test results:

```bash
./build/debug/bin/cachex_tests
```

```
[ RUN      ] new_cache_is_empty
[       OK ] new_cache_is_empty
...
158 / 158 tests passed
```

The suite covers four layers:

| Tests | What they cover |
| --- | --- |
| `cache_test`, `lru_test`, `ttl_test`, `recency_list_test` | The engine — semantics, eviction, expiry, iterator stability |
| `line_buffer_test` | Framing: a command split across reads, several commands in one read, CRLF |
| `protocol_test` | Parsing and reply formatting, plus a whole request/response cycle **with no socket involved** |
| `server_test` | Integration — a real `Server` on a real socket on an OS-assigned port |
| `concurrency_test` | Parallel readers and writers, mixed GET/SET/DELETE, many simultaneous clients |
| `sharding_test` | Shard selection and distribution, capacity split, per-shard LRU, concurrent access |
| `persistence_test` | Save/load round trips, TTL handling, malformed and missing files, saves under concurrent load |

The suite takes ~3 s, almost all of it the TTL tests sleeping. Timing tolerances
are chosen so only an order-of-magnitude stall could produce a flake; 20
consecutive runs produced none.

### Under sanitizers

The design depends on `std::list` iterators staying valid while they are held in
a hash map. A dangling iterator usually still *appears* to work, so the tests are
run under ASan/UBSan to catch what they cannot:

```bash
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build/asan -j --target cachex_tests
./build/asan/bin/cachex_tests
```

ThreadSanitizer, which is what actually matters from Stage 6 onward:

```bash
cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"
cmake --build build/tsan -j --target cachex_tests
./build/tsan/bin/cachex_tests
```

macOS has no LeakSanitizer on arm64, so the system leak checker covers that gap:

```bash
leaks --atExit -- ./build/debug/bin/cachex_tests
```

All three run clean. TSan's clean result was itself validated: pointing the same
four-thread workload at the *unsynchronised* `Cache` produces 92 data-race
reports, so the zero on `SyncCache` means something.

### Adding a test

Write the file, then add it to `tests/CMakeLists.txt`. Tests register themselves,
so `main()` is never touched.

```cpp
#include "test_framework.hpp"

CACHEX_TEST(my_test_name) {
  CHECK(1 + 1 == 2);
  CHECK_EQ(std::string("a") + "b", "ab");
}
```

## Benchmark

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DCACHEX_BUILD_BENCHMARKS=ON
cmake --build build/release -j
./build/release/bin/cachex_bench
```

Five sections, all with a fixed seed and a discarded warm-up:

1. **Core operations** — `SET insert/update`, `GET hit/miss`, `CONTAINS`, `ERASE`
2. **Cost of LRU** — one identical request sequence against an unbounded cache, a bounded cache with headroom, and a bounded cache that evicts (paired and order-alternated)
3. **Workload A** — 90% GET / 10% SET, skewed keys, cache-aside: hit-dominated
4. **Workload B** — 20% GET / 80% SET, uniform keys: eviction-dominated
5. **Hit rate vs capacity** — the point of LRU, as a curve
6. **Cost of TTL checks** — GET hits with no TTL, with a TTL, and over expired entries

**A recorded baseline is in [benchmarks/RESULTS.md](benchmarks/RESULTS.md)**, with
hardware, variance, and interpretation. The headline:

| capacity (of a 100k key space) | hit rate | evictions |
| --- | ---: | ---: |
| 1% | 3.32% | 193,372 |
| 10% | 30.92% | 138,149 |
| 40% | **84.21%** | 31,495 |
| 100% | 100.00% | 0 |

40% of the memory buys 84% of the hits, because LRU retains the entries actually
being requested. Random eviction would track capacity roughly linearly.

> ⚠️ **Behavioural numbers are exact; timings are not.**
> Hit rate, evictions, fills, and the checksum are byte-identical across runs —
> compare them freely between versions. Throughput and latency are not: latency
> percentiles are quantised to this machine's **41 ns clock tick** (a "p50 of
> 42 ns" means *one tick*), and absolute throughput is only comparable within a
> single sitting. Compare two versions back to back, and trust ratios between
> phases over absolute numbers.
>
> These numbers are also not comparable to Redis, which pays network and protocol
> costs this in-process benchmark does not.

### Network benchmark

```bash
./build/release/bin/cachex_net_bench
```

Runs a server in-process over loopback and drives it with one blocking client,
one request in flight at a time — the baseline that "multiple clients" and
"concurrent server" get compared against later.

| phase | req/sec | avg | p50 | p95 | p99 |
| --- | ---: | ---: | ---: | ---: | ---: |
| PING | ~50,100 | 19.95 µs | 19.75 µs | 24.79 µs | 30.83 µs |
| SET | ~47,500 | 21.06 µs | 20.29 µs | 26.25 µs | 32.50 µs |
| GET | ~48,450 | 20.64 µs | 20.17 µs | 25.46 µs | 31.58 µs |

**`PING` ≈ `SET` ≈ `GET`.** `PING` touches no cache data at all, so the ~0.5 µs
gap is the entire cost of the cache operation — against a ~20 µs round trip. The
transport is **97–98% of a request**. Every nanosecond won in Stages 2–4 is
invisible from the far side of a socket, which is exactly why the next stages are
about concurrency rather than micro-optimisation.

Unlike the in-process benchmark, these percentiles are *real measurements*: a
round trip is three orders of magnitude above the 41 ns clock tick.

<a id="sharding-results"></a>

### Sharding results

The benchmark runs the full matrix: 1 / 2 / 4 / 8 shards × 1 / 2 / 4 / 8 / 16
clients, identical workload throughout.

**In-process** (`ShardedCache::get()` called directly, 600k ops) — ops/sec:

| threads | 1 shard | 2 | 4 | 8 | best vs 1 |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 7,146,193 | 6,885,913 | 6,941,097 | 6,855,821 | **+0.0%** |
| 2 | 3,853,705 | 3,837,794 | 4,093,006 | 4,882,059 | **+26.7%** |
| 4 | 2,252,335 | 3,376,833 | 4,501,711 | 7,477,789 | **+232.0%** |
| 8 | 3,289,281 | 2,466,958 | 3,060,406 | 4,685,978 | **+42.5%** |
| 16 | 3,205,524 | 2,243,006 | 2,855,006 | 4,103,501 | **+28.0%** |

**Over TCP** — GET throughput moved by **≤0.8%** and p99 by **≤2%** at every
client count. Not a contradiction: a ~20 µs round trip dwarfs a ~0.4 µs cache
operation, so removing contention from 2% of a request is invisible end to end.
Stage 6's lock-free `PING` control already predicted this.

**Cost of per-shard LRU:** ≤0.02 percentage points of hit rate on a skewed
workload.

So: **sharding helps when the cache is the bottleneck, and does nothing when it
isn't.** The full tables, including SET and all p50/p95/p99 figures, are in
[benchmarks/RESULTS.md](benchmarks/RESULTS.md) and
[ARCHITECTURE.md](ARCHITECTURE.md#measured-performance-improvements).

### Concurrency scaling

The same binary measures 1 → 16 concurrent clients. `scaling` is
throughput(N clients) / throughput(1 client):

| clients | GET req/sec | scaling | p50 | p95 | p99 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 | 48,228 | 1.00x | 20.2 µs | 23.7 µs | 30.3 µs |
| 2 | 85,435 | 1.77x | 22.9 µs | 31.6 µs | 37.3 µs |
| 4 | 90,826 | 1.88x | 45.5 µs | 51.6 µs | 57.8 µs |
| 8 | 121,930 | 2.53x | 65.0 µs | 76.9 µs | 83.9 µs |
| 16 | 122,507 | 2.54x | 129.0 µs | 149.2 µs | 165.6 µs |

**Is the mutex the bottleneck? No — and the benchmark proves it.** `PING` takes
no lock and touches no cache data, yet it plateaus in the same place (2.46x at 16
clients). The ceiling over TCP is the transport and the scheduler, not the cache.

Remove the network and the picture inverts. N threads calling `SyncCache::get()`
directly scale **1.00x → 0.83x → 0.48x** at 1, 2 and 4 threads: more threads make
it *slower*, because the lock admits one at a time and the rest only add handoff
cost. The mutex is a wall the system will hit once the transport stops being the
limit — which is the argument for sharding.

Full tables and interpretation: [benchmarks/RESULTS.md](benchmarks/RESULTS.md).

Methodology — and what is deliberately *not* measured yet — is in
[ARCHITECTURE.md §13](ARCHITECTURE.md#13-benchmark-methodology).

## Persistence

Give the server a snapshot path to enable it:

```console
$ ./build/release/bin/cachex_server 6379 0 8 /var/tmp/cachex.cxs 60
CacheX 0.1.0 listening on 127.0.0.1:6379
snapshot: /var/tmp/cachex.cxs (auto-save every 60s)
```

```console
cachex> SET name ada
+OK
cachex> SAVE
:1
```

...restart the server, and the data is there. TTLs come back as the time that was
actually left, not a fresh full term.

The format is length-prefixed, so unlike the wire protocol it handles keys and
values containing spaces, newlines and NUL bytes:

```
CACHEX-SNAPSHOT 1 1757630400123
3
4 3 -1
nameada
7 3 3599976
sessiontok
```

Writes go to a temp file and are `rename()`d into place, so an interrupted save
leaves the previous snapshot intact rather than a truncated one. Loads are
all-or-nothing: a corrupt file is rejected and the cache is left untouched.

### Benchmark

```bash
./build/release/bin/cachex_persist_bench
```

| entries | save | load | snapshot | bytes/entry |
| ---: | ---: | ---: | ---: | ---: |
| 1,000 | 0.75 ms | 0.52 ms | 87.9 KiB | 90.0 |
| 10,000 | 4.55 ms | 4.67 ms | 878.9 KiB | 90.0 |
| 100,000 | 50.54 ms | 48.51 ms | 8.6 MiB | 90.0 |
| 500,000 | 264.00 ms | 252.25 ms | 42.9 MiB | 90.0 |

Both scale linearly at ~0.5 µs/entry. The snapshot is ~90 bytes/entry against
~180–200 in memory — it stores only the data, because loading rebuilds the index.

> ⚠️ **What this does not provide.** No `fsync` (a power loss can lose a save that
> reported success), no write-ahead log (everything since the last save is lost on
> a crash), no checksums, no incremental saves, no replication. It makes a cache
> survive a *planned* restart; the data must still be reconstructible from
> elsewhere. Full comparison against a real database:
> [ARCHITECTURE.md §10.7](ARCHITECTURE.md#107-what-this-is-not).

## Project layout

```
CacheX/
├── CMakeLists.txt              standard, options, warnings
├── ARCHITECTURE.md             design and reasoning (living document)
├── README.md
├── include/cachex/
│   ├── cache.hpp               public Cache API
│   ├── recency_list.hpp        Entry + RecencyList (the MRU→LRU ordering)
│   ├── line_buffer.hpp         byte stream → lines (no sockets)
│   ├── protocol.hpp            Command, parser, reply formatting
│   ├── command_handler.hpp     execute(Cache&, Command) — the bridge
│   ├── socket.hpp              RAII fd owner, send_all
│   ├── sync_cache.hpp          thread-safe wrapper: one mutex around Cache
│   ├── sharded_cache.hpp       N independently locked shards, same API
│   ├── persistence.hpp         snapshot save/load + periodic saver
│   ├── connection.hpp          one client's read/dispatch/write loop
│   ├── server.hpp              accept loop + worker threads
│   └── version.hpp.in          template → generated into the build tree
├── src/
│   ├── cache.cpp               hash map + list, eviction, expiry
│   ├── recency_list.cpp        std::list wrapper; splice-based reordering
│   ├── net/                    the network layer (cachex_net)
│   ├── main.cpp                in-process demo
│   ├── server_main.cpp         cachex_server
│   └── client_main.cpp         cachex_client
├── tests/
│   ├── test_framework.hpp      ~100 lines, no dependencies
│   ├── cache_test.cpp          cache semantics and edge cases
│   ├── lru_test.cpp            capacity, eviction, recency ordering
│   ├── ttl_test.cpp            expiry, TTL query, lazy reclamation
│   ├── recency_list_test.cpp   ordering and iterator stability
│   ├── line_buffer_test.cpp    framing: split reads, batched reads, CRLF
│   ├── protocol_test.cpp       parser + replies, no sockets
│   ├── server_test.cpp         integration over a real socket
│   ├── concurrency_test.cpp    parallel access, many clients
│   ├── sharding_test.cpp       shard selection, capacity split, per-shard LRU
│   └── persistence_test.cpp    snapshots, TTL, malformed files, concurrency
├── benchmarks/
│   ├── cache_benchmark.cpp     in-process
│   ├── net_benchmark.cpp       over TCP + sharding matrix
│   ├── persistence_benchmark.cpp  save/load timing and snapshot size
│   ├── bench_util.hpp          shared timing/percentile helpers
│   └── RESULTS.md              recorded baselines + hardware caveats
└── docs/
    └── PROTOCOL.md             the wire protocol specification
```

Two libraries, and the split is the architecture:

- **`cachex_core`** — the cache engine. Knows nothing about sockets.
- **`cachex_net`** — protocol, framing, sockets, server. Links `cachex_core`.

The dependency runs one way, and the build graph enforces it: `cachex_core` does
not link `cachex_net`, so cache code that reached for a socket would fail to
link. The payoff is that `execute(Cache&, Command)` makes the server's entire
request/response behaviour testable without opening a connection.

## Roadmap

| # | Stage | Status |
| --- | --- | --- |
| 1 | Project foundation | ✅ Done |
| 2 | Core cache (`unordered_map` + doubly linked list) | ✅ Done |
| 3 | LRU eviction | ✅ Done |
| 4 | TTL / key expiry | ✅ Done |
| 5 | TCP server + wire protocol + CLI client | ✅ Done |
| 6 | Concurrency (thread-per-connection + mutex) | ✅ Done |
| 7 | Sharded cache | ✅ Done |
| 8 | Persistence (snapshots) | ✅ Done |
| 9 | Benchmark harness II (sub-tick latency) | ⬜ |
| 10 | Active expiry | ⬜ |
| 11 | Final optimisation and benchmarking | ⬜ |

Details for each stage are in [ARCHITECTURE.md](ARCHITECTURE.md#17-future-roadmap).
