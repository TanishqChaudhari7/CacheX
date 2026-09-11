# CacheX

A Redis-inspired in-memory cache server in C++17, built from scratch to learn how
one actually works.

I wanted to understand the problems a cache has to solve rather than read about
them, so I built it in stages: hash table and LRU list, TTL, a TCP server, a wire
protocol, threading, sharding, snapshots, and a benchmark suite to check whether
any of it helped. Each stage is a separate commit with the reasoning written down.

Design notes and measurements: [ARCHITECTURE.md](ARCHITECTURE.md).
Wire protocol: [docs/PROTOCOL.md](docs/PROTOCOL.md).

## What's in it

- O(1) `GET`/`SET`/`DELETE` using a hash map plus an intrusive doubly linked list
- O(1) LRU eviction with a configurable capacity
- Per-key TTL with lazy expiration, on a monotonic clock
- A TCP server speaking a line-based text protocol, plus a CLI client
- Thread-per-connection, with the cache sharded across N independently locked shards
- Snapshot persistence (`SAVE`/`LOAD`, restore on startup)
- 212 tests, clean under AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer
- A benchmark suite covering five workloads at one to sixteen threads

No external dependencies. The test framework is about a hundred lines of header.

## Build

Needs CMake 3.16+ and a C++17 compiler.

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
```

Swap `Release` for `Debug` while working. Add `-DCACHEX_BUILD_BENCHMARKS=ON` for
the benchmarks and `-DCACHEX_WARNINGS_AS_ERRORS=ON` to fail on warnings (CI should).

## Run

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
```

The server takes `[port] [capacity] [shards] [snapshot-path] [auto-save-seconds]`.
Capacity 0 means unbounded; leave the snapshot path off to disable persistence.

It also works with netcat, which is most of the point of a text protocol:

```console
$ printf 'SET foo bar\nGET foo\nQUIT\n' | nc 127.0.0.1 6379
+OK
=bar
+BYE
```

## Protocol

One command per line, one reply per line, `\n` terminated (`\r\n` accepted). Every
reply starts with a type byte so a client can dispatch on one character.

| Request | Reply |
| --- | --- |
| `SET key value [ttl_seconds]` | `+OK` |
| `GET key` | `=value`, or `_` if absent |
| `DELETE key` | `:1` or `:0` |
| `EXISTS key` | `:1` or `:0` |
| `TTL key` | `:seconds`, `+NOEXPIRE`, or `_` |
| `SAVE` / `LOAD` | `:count` |
| `PING` | `+PONG` |
| `QUIT` | `+BYE`, then disconnect |
| anything invalid | `-ERR reason`, connection stays open |

Keys and values can't contain whitespace, since tokens are space-delimited. The
cache itself stores arbitrary bytes — it's the protocol that's limited, which is
the argument for length-prefixed framing like RESP uses. The snapshot format does
use length prefixes, so it handles anything.

## Using it as a library

```cpp
#include "cachex/sharded_cache.hpp"

cachex::ShardedCache cache(8, 100000);   // 8 shards, 100k entries total

cache.set("user:1", "ada");
cache.set("session", "abc", std::chrono::seconds(30));

if (auto v = cache.get("user:1")) {
  std::cout << *v << "\n";
}

std::string buf;
cache.get_into("user:1", buf);           // avoids the per-hit allocation
```

`Cache` is the single-threaded engine. `SyncCache` wraps it in one mutex;
`ShardedCache` splits it into independently locked shards. All three have the same
interface, so the server doesn't know or care which it's given.

A few semantics worth knowing:

- `get()` counts as a use and reorders the LRU list, so it takes an exclusive lock. Under LRU a read is a write, which is also why `shared_mutex` wouldn't buy much.
- `contains()` deliberately does *not* count as a use — otherwise `EXISTS` becomes a way to keep dead data alive forever.
- A plain `set()` clears any existing TTL, matching Redis. `set(k, v, 0)` deletes the key.
- `get()` returns a copy, not a reference. A reference would dangle as soon as another call evicted the entry.

## Tests

```bash
ctest --test-dir build/release --output-on-failure
```

212 tests covering the cache, LRU ordering, expiry, protocol parsing, framing,
the server over real sockets, concurrent access, sharding and snapshots. They run
in about three seconds, most of which is the TTL tests sleeping.

Under sanitizers:

```bash
cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"
cmake --build build/tsan -j --target cachex_tests
./build/tsan/bin/cachex_tests
```

ASan, UBSan and TSan all pass clean. Worth noting that I checked TSan actually
catches something here before trusting the clean result — pointing the same
four-thread workload at the unsynchronised `Cache` produces 92 data race reports.

## Benchmarks

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DCACHEX_BUILD_BENCHMARKS=ON
cmake --build build/release -j
./benchmarks/run_all.sh
```

That runs everything and writes the raw output to `benchmarks/results/`, along
with a record of the machine, commit and load average. Every number quoted in the
docs can be found in one of those files.

Measured on an M2 Pro, median of five runs:

| | |
| --- | --- |
| Single-threaded, 90/10 read-heavy | 6.0M ops/sec, p50 125ns, 78.95% hit rate |
| Sharded vs one global mutex, 4 threads | +175% to +235% throughput, −69% to −76% p99 |
| Removing a per-hit allocation on the read path | +45% throughput |
| Memory | 221 bytes/entry for an 80-byte payload |
| Over TCP, one client | ~48k req/sec, ~20µs round trip |

Two results I didn't expect and think are the most interesting part:

**Nothing beats the single-threaded baseline.** A cache operation costs about
170ns and touches shared memory, so coordinating threads costs more than the
parallelism buys. Sharding's real value is beating the global mutex under
concurrency, not scaling past one thread.

**Sharding does nothing over TCP.** A round trip is ~20µs against a ~0.4µs cache
operation, so the transport is 97–98% of a request. I added a lock-free `PING` to
the benchmark as a control, and it plateaus in exactly the same place as `GET` —
which is how I know the mutex wasn't the bottleneck there. All the work spent
shaving nanoseconds off the cache is invisible from the other side of a socket.

The full tables are in [benchmarks/RESULTS.md](benchmarks/RESULTS.md).

## Layout

```
include/cachex/     public headers
src/                cache engine
src/net/            protocol, sockets, connection, server
tests/              212 tests + a small test framework
benchmarks/         suite, workloads, captured results
docs/PROTOCOL.md    wire protocol spec
```

Two libraries: `cachex_core` is the engine and knows nothing about sockets;
`cachex_net` is everything network-facing and depends on it. The dependency only
runs one way, and CMake enforces it — cache code that reached for a socket
wouldn't link. That's what makes the whole request/response path testable without
opening a connection.

## Things it doesn't do

Worth being clear about, since "cache" and "database" get conflated:

- **No durability.** Snapshots aren't `fsync`ed and there's no write-ahead log, so anything since the last save is lost on a crash. It survives a planned restart, not a power cut.
- **No clustering or replication.** Single node.
- **No auth or TLS.** Assumes a trusted network, same as Redis by default.
- **One data type.** Strings only, no lists or sets.
- **Exact LRU, not approximated.** Redis samples random keys instead and skips the list entirely, which costs less memory. I wanted the O(1) structure to be visible.
- **Thread-per-connection.** Fine to a few hundred connections; an event loop is the answer beyond that.

## Roadmap

Done: foundation, core cache, LRU, TTL, TCP server, concurrency, sharding,
persistence, benchmark suite.

Next, roughly in order of what the measurements say is worth doing: an event loop
instead of thread-per-connection (that's the actual bottleneck), active expiry so
untouched expired keys get reclaimed, and `fsync` on snapshots if durability ever
matters.
