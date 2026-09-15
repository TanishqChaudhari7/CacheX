# CacheX

An in-memory key-value cache server in C++17, inspired by Redis. It serves
concurrent clients over TCP, evicts least-recently-used keys in O(1), expires
keys by TTL, splits its data across independently locked shards, and can snapshot
to disk. It has no dependencies beyond the standard library and POSIX.

Design details: [ARCHITECTURE.md](ARCHITECTURE.md) · Wire protocol:
[docs/PROTOCOL.md](docs/PROTOCOL.md) · Benchmark data:
[benchmarks/RESULTS.md](benchmarks/RESULTS.md)

## Features

- **In-memory key-value cache** — `SET`, `GET`, `DELETE`, `EXISTS`
- **O(1) average LRU eviction** — hash map plus doubly linked list, fixed capacity
- **TTL** — per-key expiry on a monotonic clock, with lazy reclamation
- **TCP protocol** — line-based text protocol, typed replies, strict input limits
- **Concurrent clients** — one worker thread per connection
- **Sharded cache** — keys hashed across independently locked shards (8 by default)
- **Persistence** — `SAVE` / `LOAD` snapshots, restore on startup, optional periodic saves
- **Benchmark suite** — five workloads across three cache versions, with committed raw results

221 tests pass under AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer.

## Architecture

```
  client ──TCP──> Server ──> worker thread per connection
                               │
                               │  LineBuffer → parse_command → execute
                               ▼
                         ShardedCache ◄────── PersistenceManager ──► snapshot file
                    ┌──────────┼──────────┐
                    ▼          ▼          ▼
                 shard 0    shard 1 … shard N-1     each shard: mutex + Cache
                                                    Cache: unordered_map + LRU list
```

The cache library knows nothing about sockets; the network layer depends on it,
never the reverse.

## Build

Requires CMake 3.16+ and a C++17 compiler (Clang, GCC 9+, or MSVC 2019+).

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
ctest --test-dir build/release --output-on-failure
```

## Run the server

```bash
./build/release/bin/cachex_server 6379
```

Arguments: `[port] [capacity] [shards] [snapshot-path] [save-interval-seconds]`.
For example, `cachex_server 6379 100000 8 cachex.snapshot 60` holds at most
100,000 entries in 8 shards and saves every minute. If Redis is already running
on 6379, pick another port.

## Run the client

```bash
./build/release/bin/cachex_client localhost 6379
```

The server also works with `nc localhost 6379`.

## Example

```
cachex> SET user:1 Tanishq
+OK
cachex> GET user:1
=Tanishq
cachex> TTL user:1
+NOEXPIRE
cachex> DELETE user:1
:1
cachex> GET user:1
_
cachex> SET session abc 60
+OK
cachex> TTL session
:60
```

Replies start with a type byte: `+` status, `=` value, `_` not found, `:` integer,
`-ERR` error.

## Benchmarks

Measured on an Apple M2 Pro (12 cores, 16 GB, macOS 26.5.2) with a Release
build. Suite figures are medians of five runs.

| Measurement | Result |
| --- | --- |
| Single-threaded cache, read-heavy workload (90% GET) | 5,147,260 ops/sec |
| `GET` over loopback TCP, one client | 47,684 req/sec, p50 20.25 µs, p99 34.12 µs |
| `GET` over TCP, 16 clients | 124,904 req/sec |
| 8 shards against one global mutex, 4 threads, read-heavy | +195.6% throughput, −72.7% p99 |
| Hit-rate cost of per-shard LRU | at most 0.02 percentage points |
| `get_into()` against `get()` | +43.6% throughput |
| Memory per entry (16-byte key, 64-byte value) | 221 bytes |
| Snapshot of 500,000 entries | save 281.53 ms, load 259.24 ms |

Two results stand out. First, over TCP a round trip takes about 20 µs, and the
cache accounts for about 0.6 µs of it, so sharding changes networked throughput
by at most 1.2%. Second, in-process, where the lock is the bottleneck, 8 shards
beat the global mutex by 150–222% at 4 threads across all five workloads.

Analysis: [ARCHITECTURE.md §14](ARCHITECTURE.md#14-performance-results). All
tables: [benchmarks/RESULTS.md](benchmarks/RESULTS.md). To reproduce:

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DCACHEX_BUILD_BENCHMARKS=ON
cmake --build build/release -j
RUNS=5 ./benchmarks/run_all.sh
```

## Design tradeoffs

- **LRU structure.** An `unordered_map` from key to a `std::list` iterator gives
  O(1) lookup, reordering and eviction. The price is memory: two allocations per
  entry and the key stored twice, because eviction starts from the list and
  needs the key to erase the map entry.
- **TTL.** Deadlines use `steady_clock`, so wall-clock changes cannot resurrect
  or mass-expire keys. Expiry is lazy: an expired key is removed when accessed.
  That needs no background thread, but a key that is never read again keeps its
  memory, and snapshots need a wall-clock timestamp to restore TTLs correctly.
- **Coarse locking.** Under LRU every `GET` reorders the list, so reads need
  exclusive access and `shared_mutex` does not help. A single mutex is simple and
  correct, but it makes the whole cache one sequential section.
- **Sharding.** Independent shard locks remove most of that contention. The
  costs are per-shard rather than global LRU, and aggregate counts that are
  approximate while writes are in flight. Over TCP the network dominates, so
  sharding matters most when the cache itself is the bottleneck.
- **Persistence.** Snapshots are written to a unique temp file and renamed into
  place, so they are always complete. They are not `fsync`ed, there is no
  write-ahead log, and a snapshot is consistent per shard rather than at one
  instant. They cover planned restarts, not crashes.

## Project layout

```
include/cachex/   public headers
src/              cache engine, persistence, server and client entry points
src/net/          protocol, sockets, connection handling, server
tests/            221 tests and a small test framework
benchmarks/       benchmark programs, run_all.sh, committed results
docs/PROTOCOL.md  wire protocol specification
```
