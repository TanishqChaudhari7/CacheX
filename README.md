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

**Stage 1 of 11 — project foundation.**

The build system, directory structure, warning configuration, and test harness
are in place and verified. The cache itself does not exist yet: the executable
prints its version and exits.

| | |
| --- | --- |
| ✅ Done | CMake build, Debug/Release configs, strict warnings, test framework wired to CTest |
| ⬜ Next | Stage 2 — the core cache (`GET` / `SET` / `DEL` over a hash table) |
| ⬜ Later | LRU eviction · TTL · benchmarks · TCP server · protocol · concurrency · sharding · persistence · optimisation |

---

## Requirements

- A C++17 compiler — Apple Clang, GCC 9+, or MSVC 2019+
- CMake 3.16 or newer

No other dependencies. Nothing is fetched at configure time.

## Build

CacheX uses out-of-source builds: all output goes into `build/`, which is
gitignored. Configure once per build type, then build as often as you like.

**Debug** — no optimisation, full debug info, assertions active. Use while developing.

```bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug -j
```

**Release** — `-O3 -DNDEBUG`. Use for anything you intend to measure; benchmarking
a Debug build measures the absence of the optimiser, not your code.

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
```

### Build options

| Option | Default | Effect |
| --- | --- | --- |
| `CACHEX_BUILD_TESTS` | `ON` | Build the test suite |
| `CACHEX_BUILD_BENCHMARKS` | `OFF` | Build benchmarks (none exist yet) |
| `CACHEX_WARNINGS_AS_ERRORS` | `OFF` | Turn warnings into errors — recommended while developing and in CI |

```bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug -DCACHEX_WARNINGS_AS_ERRORS=ON
```

## Run

```bash
./build/debug/bin/cachex
```

```
CacheX 0.1.0
Foundation build: no cache engine and no server yet.
```

## Test

Through CTest:

```bash
ctest --test-dir build/debug --output-on-failure
```

Or run the test binary directly, which prints per-test results:

```bash
./build/debug/bin/cachex_tests
```

```
[ RUN      ] version_string_matches_version_constants
[       OK ] version_string_matches_version_constants
[ RUN      ] version_string_has_three_components
[       OK ] version_string_has_three_components

2 / 2 tests passed
```

### Adding a test

Write the file, then add it to `tests/CMakeLists.txt`. Tests register themselves,
so `main()` never needs to be touched.

```cpp
#include "test_framework.hpp"

CACHEX_TEST(my_test_name) {
  CHECK(1 + 1 == 2);
  CHECK_EQ(std::string("a") + "b", "ab");
}
```

## Project layout

```
CacheX/
├── CMakeLists.txt        top-level build: standard, options, warnings
├── ARCHITECTURE.md       design and reasoning (living document)
├── README.md             this file
├── .gitignore
├── include/cachex/       public headers
├── src/                  implementation: cachex_core library + main.cpp
├── tests/                test framework and test files
├── benchmarks/           placeholder; off by default
└── docs/                 longer-form notes
```

All logic lives in the `cachex_core` library; `main.cpp` is a thin wrapper around
it. That split is what lets the tests link against exactly the code the server
runs, rather than a second copy of it.

## Roadmap

| # | Stage | Status |
| --- | --- | --- |
| 1 | Project foundation | ✅ Done |
| 2 | Core cache (hash table, `GET`/`SET`/`DEL`) | ⬜ |
| 3 | LRU eviction | ⬜ |
| 4 | TTL / key expiry | ⬜ |
| 5 | Benchmark harness | ⬜ |
| 6 | TCP server | ⬜ |
| 7 | Client protocol | ⬜ |
| 8 | Concurrency | ⬜ |
| 9 | Sharded cache | ⬜ |
| 10 | Persistence | ⬜ |
| 11 | Final optimisation and benchmarking | ⬜ |

Details for each stage are in [ARCHITECTURE.md](ARCHITECTURE.md#6-future-roadmap).
