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

**Stage 3 of 11 — LRU eviction.**

The cache enforces a fixed capacity and evicts the least recently used entry in
**O(1)** — no scan to find a victim. 72 tests pass, including under
AddressSanitizer/UndefinedBehaviorSanitizer and with zero leaks. The benchmark
harness measures two workload types with latency percentiles and a hit-rate curve.

There is still **no expiry, no networking, and no concurrency** — the cache is
single-threaded by design at this stage.

| | |
| --- | --- |
| ✅ Stage 1 | CMake build, Debug/Release, strict warnings, test framework |
| ✅ Stage 2 | Core cache (`unordered_map` + `std::list`), benchmark baseline |
| ✅ Stage 3 | Fixed capacity, O(1) LRU eviction, 72 tests, workload benchmarks |
| ⬜ Next | Stage 4 — per-key TTL |
| ⬜ Later | benchmark harness II · TCP server · protocol · concurrency · sharding · persistence · optimisation |

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
| `erase` | ➖ n/a | Frees a slot, so the next insert need not evict |

**Not thread-safe.** One thread at a time; locking arrives in Stage 8.

---

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
72 / 72 tests passed
```

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

macOS has no LeakSanitizer on arm64, so the system leak checker covers that gap:

```bash
leaks --atExit -- ./build/debug/bin/cachex_tests
```

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

Methodology — and what is deliberately *not* measured yet — is in
[ARCHITECTURE.md §8](ARCHITECTURE.md#8-benchmark-methodology).

## Project layout

```
CacheX/
├── CMakeLists.txt              standard, options, warnings
├── ARCHITECTURE.md             design and reasoning (living document)
├── README.md
├── include/cachex/
│   ├── cache.hpp               public Cache API
│   ├── recency_list.hpp        Entry + RecencyList (the MRU→LRU ordering)
│   └── version.hpp.in          template → generated into the build tree
├── src/
│   ├── cache.cpp               hash map + list, kept in sync; eviction
│   ├── recency_list.cpp        std::list wrapper; splice-based reordering
│   ├── version.cpp
│   └── main.cpp                thin demo; becomes the server in Stage 6
├── tests/
│   ├── test_framework.hpp      ~100 lines, no dependencies
│   ├── cache_test.cpp          cache semantics and edge cases
│   ├── lru_test.cpp            capacity, eviction, recency ordering
│   ├── recency_list_test.cpp   ordering and iterator stability
│   └── version_test.cpp
├── benchmarks/
│   ├── cache_benchmark.cpp
│   └── RESULTS.md              recorded baseline + hardware caveats
└── docs/
```

All logic lives in the `cachex_core` library; `main.cpp`, the tests, and the
benchmark are thin consumers of it. That split is what lets the tests exercise
exactly the code the server will run, rather than a second copy of it.

## Roadmap

| # | Stage | Status |
| --- | --- | --- |
| 1 | Project foundation | ✅ Done |
| 2 | Core cache (`unordered_map` + doubly linked list) | ✅ Done |
| 3 | LRU eviction | ✅ Done |
| 4 | TTL / key expiry | ⬜ |
| 5 | Benchmark harness II (sub-tick latency) | ⬜ |
| 6 | TCP server | ⬜ |
| 7 | Client protocol | ⬜ |
| 8 | Concurrency | ⬜ |
| 9 | Sharded cache | ⬜ |
| 10 | Persistence | ⬜ |
| 11 | Final optimisation and benchmarking | ⬜ |

Details for each stage are in [ARCHITECTURE.md](ARCHITECTURE.md#12-future-roadmap).
