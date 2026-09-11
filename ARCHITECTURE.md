# CacheX — Architecture

> Living document. It is updated at every stage of the project.
> **Current stage: 1 — project foundation.**

---

## 1. Project Goal

CacheX is a small, Redis-inspired in-memory key–value cache server written in C++17.

**The problem a cache solves.** Reading from a database costs milliseconds; reading
from RAM costs microseconds. Most workloads are heavily skewed — a small fraction
of the keys serve most of the requests. A cache keeps that hot subset in memory in
front of the slow store, so the common request never reaches the database.

That single idea forces every hard question this project is built to explore:

- Memory is finite, so **what do you evict** when the cache is full?
- Cached data goes stale, so **when does an entry expire**?
- Many clients arrive at once, so **how do concurrent readers and writers share the data** without corrupting it or serialising everything behind one lock?
- Clients live in other processes, so **how is the cache exposed over a network** and what protocol do they speak?

CacheX is deliberately small enough that every one of those answers can be read,
understood, and defended line by line.

**Non-goal.** This is not a Redis clone. Redis has hundreds of commands, multiple
data structures, clustering, replication, scripting, and modules. CacheX implements
the handful of things that demonstrate the concepts, and nothing more.

---

## 2. Scope

### In scope

| Area | What it covers |
| --- | --- |
| Core cache | `GET` / `SET` / `DEL` / `EXISTS` over string keys and values |
| Eviction | Bounded capacity with an LRU policy, O(1) per operation |
| Expiry | Per-key TTL, with a defined expiry strategy (lazy and/or active) |
| Networking | A single-node TCP server with a simple line-based text protocol |
| Concurrency | Multiple clients served safely and, where possible, in parallel |
| Sharding | The cache split into independently locked shards to cut contention |
| Persistence | A basic snapshot/restore so state survives a restart |
| Measurement | A benchmark harness reporting throughput, latency percentiles, and hit rate |

### Explicitly out of scope

These are excluded on purpose, so the project stays finishable and explainable.
Each one is a good interview answer in its own right — *"here is why I did not build it"*.

- **Clustering, replication, failover.** Distributed consensus is a project of its own; single-node already exercises the data structures and concurrency.
- **Rich data types** (lists, sets, sorted sets, hashes). They add surface area, not insight — the interesting problems are in eviction, expiry, and concurrency.
- **Authentication, TLS, multi-tenancy.** CacheX assumes a trusted local network, exactly as Redis does by default. It should not be exposed to the public internet.
- **Lua scripting, pub/sub, transactions.** Orthogonal to the caching problem.
- **Hand-written lock-free data structures.** Enormous difficulty-to-benefit ratio, and nearly impossible to defend confidently under interview pressure. Sharded locking gets most of the scalability at a fraction of the risk.

---

## 3. High-Level Architecture

The target design. Only the outer shell exists today — see §4.

```
                    Client
                      |
                      |  TCP, line-based text protocol
                      v
             +--------------------+
             |   CacheX Server    |   accept loop, per-connection handling
             +--------------------+
                      |
                      v
             +--------------------+
             |   Command Parser   |   bytes  ->  typed command
             +--------------------+
                      |
                      v
             +--------------------+
             |    Cache Engine    |   the actual cache semantics
             +--------------------+
                      |
        +-------------+-------------+
        |             |             |
        v             v             v
   Hash Table    LRU Policy    TTL Management
   O(1) lookup   O(1) recency  expiry deadlines
   key -> node   intrusive     per entry
                 linked list
```

**Why these three pieces sit side by side.** The hash table answers *"where is this
key?"* in O(1). The LRU list answers *"what is least recently used?"* in O(1). Each
is fast at its own job and useless at the other's — a hash table has no notion of
order, and a linked list cannot search. The classic LRU cache is the two structures
combined: the hash map stores keys pointing at *nodes inside the list*, so a lookup
finds the node in O(1) and moving that node to the front of the list is a constant
number of pointer writes. TTL is a third axis over the same entries: an expiry
deadline stored per entry, checked on access and, later, swept in the background.

---

## 4. Current Components

Everything that exists after Stage 1. There is no cache and no server yet — this
stage builds the scaffolding that every later stage plugs into.

| Path | Purpose |
| --- | --- |
| `CMakeLists.txt` | Top-level build: language standard, build options, warning flags, subdirectories. |
| `include/cachex/` | Public headers — the interface other code (and the tests) compile against. Only `version.hpp.in` today. |
| `src/` | Implementation. Contains `cachex_core` (the library) and `main.cpp` (the executable). |
| `tests/` | The test framework header, the test entry point, and one test file. |
| `benchmarks/` | Placeholder; no targets until there is a cache to measure. Off by default. |
| `docs/` | Longer-form notes that would bloat this file: benchmark write-ups, protocol specs, rejected designs. |
| `ARCHITECTURE.md` | This document — what the system is and why. |
| `README.md` | How to build, run, and test it. |
| `.gitignore` | Keeps build output out of version control. |

### Build targets

```
cachex_warnings  (INTERFACE)  warning flags, carried as a target rather than global flags
        |
        +--> cachex_core  (static library)  all real logic lives here
                    |
                    +--> cachex        (executable)  thin main()
                    +--> cachex_tests  (executable)  links the same library
```

**The library/executable split is the most important structural decision in this
stage.** A `main()` function cannot be linked into a test binary — there would be
two `main`s. If the logic lived in `main.cpp` it would be untestable, and the
usual workaround is for tests to `#include` the `.cpp` file or recompile the
sources a second time, which invites the two copies to drift. Putting the logic in
`cachex_core` means the tests exercise *exactly* the object code the server runs.

---

## 5. Design Decisions

### 5.1 Why C++17 rather than C++20

**Chosen: C++17.**

C++17 already contains everything this project needs:

- `std::optional<T>` — the natural return type for a cache lookup that may miss, with no sentinel values or out-parameters.
- `std::string_view` — parse a command without copying the bytes off the socket buffer.
- `std::shared_mutex` — readers-writer locking, which matters for a read-heavy cache.
- Structured bindings and `if (auto it = map.find(k); it != map.end())` — the map lookups in an LRU implementation read far better with them.

It is also **available everywhere without effort**: Apple Clang, GCC 9+, and MSVC
2019 all support it fully. Anyone cloning this repository can build it with the
compiler already on their machine.

What C++20 would have added, and why none of it justifies the cost here:

| C++20 feature | Why it is not needed |
| --- | --- |
| Coroutines | The one genuine temptation, for async networking. But an explicit `epoll`/`kqueue` event loop makes the I/O model *visible* — and being able to explain readiness-based I/O is worth more in an interview than hiding it behind `co_await`. |
| Concepts / ranges | Real improvements to generic code. CacheX has almost no generic code. |
| `std::format` | Convenience only, and library support is still uneven across compilers. |
| `std::jthread` | Saves a `join()`. Not worth a toolchain constraint. |

The honest trade-off: C++17 costs us `std::span` and `std::format`, both easy to
live without. The deciding factor is that **every C++17 feature used here can be
explained in one sentence**, which is exactly the bar this project sets.

### 5.2 Why CMake

It is the de-facto standard for C++, so it is the build system a reviewer expects
and the one that IDEs, `clangd`, sanitizers, and CI already understand. It is also
cross-platform out of the box, and it makes the Debug/Release split a
configuration flag rather than a hand-maintained set of compiler invocations.

The alternatives were considered and rejected: a hand-written `Makefile` means
writing header-dependency tracking by hand, and Bazel/Meson are fine tools that
most reviewers do not have installed.

The CMake here is written in the **modern, target-based style** — properties are
attached to targets (`target_include_directories`, `target_link_libraries`) rather
than set as directory-wide globals. The practical payoff: the include path and the
warning flags travel with the target that needs them, so nothing leaks into a
dependency, and a new target gets the right flags by linking rather than by
copying a variable.

### 5.3 Why warnings are an INTERFACE target

`cachex_warnings` carries no code — only flags. Targets opt in by linking it.

The alternative, appending to the global `CMAKE_CXX_FLAGS`, applies the flags to
*everything* the build compiles, including any third-party library added later.
That library's warnings are not ours to fix, and hundreds of them scrolling past
is exactly how a real warning in our own code gets missed.

`-Werror` is available (`-DCACHEX_WARNINGS_AS_ERRORS=ON`) but **off by default**,
so that a new compiler version with a new warning cannot break someone's clone.
CI and local development should turn it on.

The less obvious flags in the set earn their place:

- `-Wshadow` — a local variable silently hiding a member is a classic source of "the assignment did nothing".
- `-Wconversion` / `-Wsign-conversion` — a cache is full of `size_t` capacities and `int` counters; silent narrowing and signed/unsigned mixups are where the real bugs hide.
- `-Wold-style-cast` — forces `static_cast` and friends, which are checked by the compiler and greppable by a human.
- `-Wnon-virtual-dtor` — deleting a derived object through a base pointer with a non-virtual destructor is undefined behaviour, and this catches it at compile time.

### 5.4 Why the test framework is hand-written

Roughly 100 lines of header, no dependency. Only three capabilities are needed:
register a test, assert a condition, exit non-zero on failure.

Pulling in GoogleTest or Catch2 would make the test framework the largest thing in
the repository by an order of magnitude, and would add a fetch step between a
clone and a build. The framework is wired into CTest, so `ctest` works normally,
and the macro names (`CACHEX_TEST`, `CHECK`, `CHECK_EQ`) deliberately mirror
Catch2's — if the suite outgrows this, swapping frameworks is a contained change.

One non-obvious implementation detail: the test registry is a **function-local
static**, not a namespace-scope global. Tests in different `.cpp` files register
themselves before `main()` runs, and the initialisation order across translation
units is unspecified — a namespace-scope registry could be registered *into*
before it was constructed. A function-local static is guaranteed to be constructed
on first use, which removes the problem entirely. This is the *static
initialisation order fiasco*, and it is a common interview question.

### 5.5 Why the version header is generated

`configure_file()` expands `include/cachex/version.hpp.in` into the build
directory, substituting the version from the `project()` call.

Without it the version string exists in two places — `CMakeLists.txt` and a
header — and the two drift the first time someone bumps one and forgets the
other. The generated header is written to the **build** tree, never the source
tree, so a clean checkout is never polluted by build output.

### 5.6 Why `build/` is not committed

Build output is fully reproducible from the sources, is specific to one compiler
and one machine, and goes stale the instant a flag changes. Committing it also
makes every diff unreadable. The rule: commit the inputs, never the outputs.

---

## 6. Future Roadmap

**Everything in this section is future work. None of it is implemented.**

| # | Stage | What it adds | Core concepts it demonstrates |
| --- | --- | --- | --- |
| 2 | **Core cache** | `GET` / `SET` / `DEL` over a hash table, fixed capacity | Hash tables, collisions, load factor, move semantics, RAII |
| 3 | **LRU eviction** | Hash map + intrusive doubly-linked list, O(1) eviction | The classic two-structure design; why O(1) needs both |
| 4 | **TTL** | Per-key expiry deadlines | Lazy vs. active expiry; `steady_clock` vs. `system_clock` |
| 5 | **Benchmark harness** | Throughput, p50/p99 latency, hit rate | Measuring before optimising; why the average latency lies |
| 6 | **TCP server** | `socket` / `bind` / `listen` / `accept`, one client at a time | The socket API; blocking I/O; partial reads |
| 7 | **Client protocol** | A line-based text protocol and its parser | Framing, buffering, malformed-input handling |
| 8 | **Concurrency** | Many clients at once — thread pool or event loop | Data races, mutexes, `shared_mutex`, deadlock avoidance |
| 9 | **Sharded cache** | N independently locked shards, keys hashed to a shard | Lock contention as the real bottleneck; scaling reads and writes |
| 10 | **Persistence** | Snapshot to disk and restore on startup | Serialisation; durability vs. throughput; crash-safe writes |
| 11 | **Final optimisation** | Profile-guided tuning, then re-benchmark | Cache locality, allocation cost, proving an improvement with numbers |

Each stage ends with this document updated: new components in §4, new decisions in
§5, and the diagram in §3 grown to match what actually exists.
