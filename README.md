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

**Stage 2 of 11 — core single-threaded cache.**

The cache itself works: `set` / `get` / `erase` / `contains` / `size`, backed by a
hash map for O(1) lookup and a doubly linked list for recency ordering. 48 tests
pass, including under AddressSanitizer and UndefinedBehaviorSanitizer. There is a
benchmark with a recorded baseline.

There is still **no eviction, no expiry, no networking, and no concurrency** — the
cache is unbounded and single-threaded by design at this stage.

| | |
| --- | --- |
| ✅ Stage 1 | CMake build, Debug/Release, strict warnings, test framework |
| ✅ Stage 2 | Core cache (`unordered_map` + `std::list`), 48 tests, benchmark baseline |
| ⬜ Next | Stage 3 — LRU eviction with a capacity limit |
| ⬜ Later | TTL · percentile benchmarks · TCP server · protocol · concurrency · sharding · persistence · optimisation |

## API

```cpp
#include "cachex/cache.hpp"

cachex::Cache cache;

cache.set("user:1", "ada");             // insert or overwrite
cache.set("user:1", "ada lovelace");    // overwrite; size stays 1

if (auto value = cache.get("user:1")) { // std::optional -- nullopt on a miss
  std::cout << *value << "\n";          // a copy, not a reference into the cache
}

cache.contains("user:1");               // true  -- does not count as a "use"
cache.erase("user:1");                  // true  -- false if the key was absent
cache.size();                           // 0
```

All of `set`, `get`, `erase`, `contains`, and `size` are **O(1) average**
(O(n) worst case on hash collisions). Empty keys and empty values are both legal,
and an empty value is distinguishable from a missing key — that is why `get`
returns `std::optional`. Keys and values are length-delimited `std::string`, so
embedded null bytes are fine.

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

  get user:1  -> ada lovelace
  get user:2  -> grace
  get user:42 -> (nil)
  contains user:2 -> true
  erase user:2    -> true
  erase user:2    -> false
  size            -> 1
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
48 / 48 tests passed
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

Measures throughput and average latency for `SET insert`, `SET update`,
`GET hit`, `GET miss`, `CONTAINS`, and `ERASE` — 200,000 operations per phase,
fixed seed, median of 5 runs after a discarded warm-up.

**A recorded baseline is in [benchmarks/RESULTS.md](benchmarks/RESULTS.md)**, with
the hardware it was measured on and the observed run-to-run variance.

> ⚠️ **Benchmark numbers depend entirely on hardware, compiler, and system state.**
> The recorded results came from one specific machine (Apple M2 Pro, macOS,
> AppleClang 21). Your numbers *will* differ.
>
> They differ on the *same* machine too: across 12 runs of the identical binary,
> the memory-bound phases split into two clusters **~2× apart**. So compare two
> versions of CacheX only by running them back to back in the same sitting, and
> trust the *ratios between phases* rather than absolute throughput. The evidence
> and the leading explanation are in [benchmarks/RESULTS.md](benchmarks/RESULTS.md).
>
> These numbers are also not comparable to Redis, which pays network and protocol
> costs this in-process benchmark does not.

Methodology — and what is deliberately *not* measured yet — is in
[ARCHITECTURE.md §7](ARCHITECTURE.md#7-benchmark-methodology).

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
│   ├── cache.cpp               hash map + list, kept in sync
│   ├── recency_list.cpp        std::list wrapper; splice-based reordering
│   ├── version.cpp
│   └── main.cpp                thin demo; becomes the server in Stage 6
├── tests/
│   ├── test_framework.hpp      ~100 lines, no dependencies
│   ├── cache_test.cpp          cache semantics and edge cases
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
| 3 | LRU eviction | ⬜ |
| 4 | TTL / key expiry | ⬜ |
| 5 | Benchmark harness II (percentiles, hit rate) | ⬜ |
| 6 | TCP server | ⬜ |
| 7 | Client protocol | ⬜ |
| 8 | Concurrency | ⬜ |
| 9 | Sharded cache | ⬜ |
| 10 | Persistence | ⬜ |
| 11 | Final optimisation and benchmarking | ⬜ |

Details for each stage are in [ARCHITECTURE.md](ARCHITECTURE.md#10-future-roadmap).
