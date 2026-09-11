# CacheX — Architecture

> Living document. It is updated at every stage of the project.
> **Current stage: 2 — core single-threaded cache.**

| Legend | |
| --- | --- |
| ✅ **Implemented** | exists, is tested, and is described here as it actually behaves |
| ⬜ **Future work** | described only so today's design can be judged against where it is going |

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

| Area | What it covers | Status |
| --- | --- | --- |
| Core cache | `set` / `get` / `erase` / `contains` / `size` over string keys and values | ✅ Stage 2 |
| Measurement | Benchmark harness: throughput and average latency | ✅ Stage 2 (percentiles ⬜ Stage 5) |
| Eviction | Bounded capacity with an LRU policy, O(1) per operation | ⬜ Stage 3 |
| Expiry | Per-key TTL, with a defined expiry strategy | ⬜ Stage 4 |
| Networking | Single-node TCP server, line-based text protocol | ⬜ Stages 6–7 |
| Concurrency | Multiple clients served safely and, where possible, in parallel | ⬜ Stage 8 |
| Sharding | Cache split into independently locked shards | ⬜ Stage 9 |
| Persistence | Snapshot/restore so state survives a restart | ⬜ Stage 10 |

### Explicitly out of scope

Excluded on purpose, so the project stays finishable and explainable. Each is a
good interview answer in its own right — *"here is why I did not build it"*.

- **Clustering, replication, failover.** Distributed consensus is a project of its own; single-node already exercises the data structures and concurrency.
- **Rich data types** (lists, sets, sorted sets, hashes). Surface area, not insight — the interesting problems are eviction, expiry, and concurrency.
- **Authentication, TLS, multi-tenancy.** CacheX assumes a trusted local network, exactly as Redis does by default. It should not be exposed to the public internet.
- **Lua scripting, pub/sub, transactions.** Orthogonal to the caching problem.
- **Hand-written lock-free data structures.** Enormous difficulty-to-benefit ratio, and nearly impossible to defend confidently under pressure. Sharded locking gets most of the scalability at a fraction of the risk.

---

## 3. High-Level Architecture

```
                    Client
                      |
                      |  ⬜ TCP, line-based text protocol        (Stage 6-7)
                      v
             +--------------------+
             |   CacheX Server    |  ⬜ accept loop, connections  (Stage 6)
             +--------------------+
                      |
                      v
             +--------------------+
             |   Command Parser   |  ⬜ bytes -> typed command    (Stage 7)
             +--------------------+
                      |
======================|====================== everything below is ✅ implemented
                      v
        +-------------------------------+
        |          Cache                |   cache.hpp / cache.cpp
        |  set · get · erase            |
        |  contains · size · clear      |
        +-------------------------------+
             |                    |
             |  owns              |  borrows (iterators)
             v                    v
   +--------------------+   +----------------------------+
   |    RecencyList     |<--|      index_                |
   |  std::list<Entry>  |   |  unordered_map<            |
   |                    |   |    string,                 |
   |  MRU ... LRU       |   |    RecencyList::Iterator>  |
   |  head       tail   |   +----------------------------+
   +--------------------+     "where is this key?"  O(1)
     "what is oldest?"  O(1)
             |
             +--> ⬜ LRU eviction hook  (Stage 3: oldest() / pop_oldest())
             +--> ⬜ TTL deadline field (Stage 4: Entry gains an expiry time)
```

### How the two structures cooperate

```
  index_ (hash map)                    entries_ (doubly linked list)
  ----------------                     -----------------------------
  "user:2"  --------------------+
  "user:1"  ------------+       |
  "user:9"  ---+        |       |
               |        |       |
               v        v       v
  head  [user:9] <-> [user:1] <-> [user:2]  tail
        (newest)                  (oldest)
```

Each map value is a `std::list` iterator pointing directly at that key's node.
A lookup is one hash (O(1)); promoting the node it found to the head is a splice
(O(1)). Neither structure can do the other's job — a hash map has no ordering,
and a linked list cannot search — so the cache is the pair of them.

---

## 4. Cache Design

### 4.1 Class layout

Two classes, deliberately not one:

```cpp
struct Entry {                  // recency_list.hpp
  std::string key;
  std::string value;            // ⬜ Stage 4 adds an expiry deadline here
};

class RecencyList {             // owns the data, maintains MRU -> LRU order
  std::list<Entry> entries_;
 public:
  Iterator insert_newest(std::string key, std::string value);
  void mark_used(Iterator it);
  void erase(Iterator it);
  const Entry& oldest() const;  // ⬜ the Stage 3 eviction hook
  void pop_oldest();            // ⬜ the Stage 3 eviction hook
};

class Cache {                   // the public API; owns the index
  RecencyList entries_;
  std::unordered_map<std::string, RecencyList::Iterator> index_;
};
```

**Why split them at all?** `RecencyList` is the *eviction policy*, and it is the
part most likely to change: Stage 3 turns it into LRU, and a plausible later
variant is LFU or a segmented LRU. Keeping it behind six named operations means
`Cache` never touches `std::list` directly, so swapping the policy touches one
file. It also makes the ordering invariant testable on its own
(`tests/recency_list_test.cpp`), separately from cache semantics.

It is a thin class on purpose. It is **not** an abstract base class, there is no
policy template, and there is no factory — none of that is needed to swap one
concrete implementation for another at this size, and all of it would cost more
in indirection than it returns.

### 4.2 The algorithms

| Operation | What actually happens |
| --- | --- |
| `set` (new key) | Hash the key → miss. Push a new node at the head. Insert `key → iterator` into the map. |
| `set` (existing) | Hash → hit. Move-assign the new value into the existing node. Splice the node to the head. **The node is never destroyed, so the iterator in the map stays valid.** |
| `get` (hit) | Hash → hit. Splice the node to the head. Return a copy of the value. |
| `get` (miss) | Hash → miss. Return `std::nullopt`. Nothing is inserted, nothing is reordered. |
| `erase` | Hash → hit. Erase the list node **first** (the map entry is what tells us which node), then erase the map entry. |
| `contains` | Hash → hit or miss. Deliberately does *not* reorder — see §4.4. |
| `size` | `index_.size()`. |

### 4.3 Why `std::unordered_map`

The cache's defining operation is "given a key, find its value, fast". That is
exactly a hash table: average O(1), independent of how many keys are stored.

- **vs. `std::map` (red-black tree):** O(log n) instead of O(1), and it would demand that keys be *ordered*, which a cache has no use for. At 1,000,000 keys that is ~20 comparisons of full strings versus one hash. The only thing `std::map` would buy us is range queries, which CacheX does not have.
- **vs. `std::vector` + linear scan:** O(n). Fine for ten keys, unusable at a hundred thousand.
- **vs. a hand-written hash table:** the interesting part of this project is the *combination* of structures and the eviction policy, not re-implementing open addressing. `std::unordered_map` is well-tested and everyone reading the code already knows its semantics. Stage 11 can revisit this with measurements in hand — its node-per-element layout is genuinely cache-unfriendly, and that is a real, *measurable* argument to make later rather than a guess to make now.

**Its weaknesses, stated honestly** — these are the follow-up questions:

- O(1) is *average*, not worst case. Adversarial keys that all hash to one bucket degrade it to O(n). Real caches on untrusted input mitigate this with a randomly seeded hash; CacheX assumes a trusted network (§2), so it does not.
- It is node-based: every element is a separate allocation, and traversal chases pointers. This shows up clearly in the Stage 2 benchmark (§7).
- Growth rehashes every element, so an individual `set` can be O(n) even though the amortised cost is O(1).

### 4.4 Why a doubly linked list

The list answers a question the hash map cannot: **which entry has gone longest
without being used?** It is the structure that makes O(1) eviction possible.

Three properties matter, and `std::list` is close to the only standard container
with all three:

1. **O(1) splice.** `entries_.splice(begin, entries_, it)` moves a node to the front by rewriting a handful of pointers. Nothing is copied, nothing is allocated, and no other element moves. This is the single operation the entire LRU design is built on.
2. **Iterator and reference stability.** Inserting or erasing any element leaves iterators to *every other* element valid. This is what lets the hash map store an iterator per key and trust it indefinitely. `std::vector` fails here outright — one reallocation and every stored iterator dangles.
3. **O(1) access to both ends.** The head is the most recently used, the tail is the least. Eviction is `pop_oldest()`.

**Why *doubly* and not singly linked.** To unlink a node you must reach its
predecessor. In a singly linked list that means walking from the head — O(n),
which destroys the whole point. A `prev` pointer makes removal O(1) from an
iterator you already hold. The extra pointer per node is the price of O(1)
eviction, and it is worth it.

**Why not `std::deque`?** O(1) at both ends, but no O(1) removal from the middle,
and insertion invalidates iterators. Recency updates are middle removals.

### 4.5 Return types

`get` returns `std::optional<std::string>`, not `std::string`.

A plain `std::string` cannot distinguish "the key is absent" from "the key holds
an empty string" — both would be `""`. Empty values are legal in CacheX
(`tests/cache_test.cpp: empty_value_is_distinct_from_a_missing_key` pins this),
so the return type has to carry presence separately from the value. The
alternatives — a sentinel value, or a `bool get(key, string& out)` out-parameter
— either constrain what can be stored or make every call site two statements.

`contains` returns `bool` and, unlike `get`, **does not count as a use**. A key
that is only ever peeked at should still age out; otherwise `EXISTS` becomes a
way to keep dead data alive forever. That decision is also what makes `contains`
`const` while `get` cannot be — a consequence worth noticing rather than fighting.

---

## 5. Complexity

All bounds assume a hash function that distributes keys reasonably.

| Operation | Average | Worst case | Where the worst case comes from |
| --- | --- | --- | --- |
| `set` (insert) | **O(1)** | O(n) | All keys collide into one bucket; or the insert triggers a rehash |
| `set` (update) | **O(1)** | O(n) | Bucket collisions only — no rehash, no allocation |
| `get` | **O(1)** | O(n) | Bucket collisions. The splice is always O(1) |
| `erase` | **O(1)** | O(n) | Bucket collisions |
| `contains` | **O(1)** | O(n) | Bucket collisions |
| `size` | **O(1)** | O(1) | `unordered_map` keeps a running count |
| `empty` | **O(1)** | O(1) | |
| `clear` | **O(n)** | O(n) | Every node must be destroyed |
| `keys_by_recency` | **O(n)** | O(n) | Diagnostic only; walks the list and copies every key |

**Space: O(n).** Per entry: the key twice (see §6), the value once, two list
pointers, and the map's node and bucket overhead.

A caveat that matters more than the table: **every operation here is O(1), and
they still differ by roughly 10× in measured cost** (§7). The constant factors —
allocation, copying, and memory locality — dominate at this scale. The table is
the right answer to "how does this scale?" and the wrong answer to "which is
fastest?".

---

## 6. Ownership and Lifetime

This is where a cache of this shape goes wrong, so it is worth being precise.

### One owner, many borrowers

**`RecencyList::entries_` owns every `Entry`.** The hash map stores
`std::list<Entry>::iterator` — a non-owning handle. Nothing is reference-counted,
nothing is shared, and there is not a single raw `new`, `delete`, or smart
pointer in the cache. The list node *is* the allocation, and the list's
destructor is what frees it.

The members are declared in this order:

```cpp
RecencyList entries_;                                            // owner
std::unordered_map<std::string, RecencyList::Iterator> index_;   // borrower
```

Members are destroyed in reverse declaration order, so the borrower (`index_`) is
destroyed before the owner (`entries_`). Nothing ever observes an iterator into a
destroyed list.

### The invariant

> Every iterator in `index_` points to a live node in `entries_`, and every node
> in `entries_` is pointed to by exactly one entry in `index_`.

Both structures must therefore be updated together, and `erase` is the one place
the order is not interchangeable:

```cpp
entries_.erase(it->second);   // read it->second while the map entry still exists
index_.erase(it);             // only now drop the handle
```

Erasing the map entry first would destroy the iterator being used to find the
node. This is a genuine use-after-free, and it is exactly the kind of bug that
`-fsanitize=address` catches — which is why the suite is run under ASan/UBSan.

### Why the key is stored twice

`Entry::key` duplicates the map's key. That is a real cost — roughly 16–32 extra
bytes per entry — and it is deliberate.

Eviction runs *list → map*: take the oldest node, then remove its key from the
index. Without the key in the node there is no way to find the map entry to
erase, and the only alternative would be scanning the map: O(n), which defeats
the purpose.

⬜ *Future (Stage 11):* the node could instead hold `const std::string*` pointing
at the map's key, which is stable across rehashing because `unordered_map`
rehashing invalidates *iterators* but not *pointers or references* to elements.
That removes the duplication at the cost of a subtlety that has to be explained
every time someone reads the code. It is a measured optimisation for later, not
a default.

### Why `get` returns a copy

Returning `const std::string&` or a pointer would be faster — the benchmark puts
the copy at roughly 215 ns per hit (§7) — and it would be a trap. The reference
stays valid only until the next call that touches that entry, and from Stage 3
onward *any* `set` can evict *any* entry. A caller holding a reference across two
calls would read freed memory, and it would usually appear to work.

The safe default is a copy. Once there is a network server, the value is
serialised to a socket buffer immediately anyway, so the copy largely disappears
into work that has to happen regardless.

⬜ *Future:* `std::shared_ptr<const std::string>` values would make sharing safe
and cheap without copying the bytes, at the cost of an atomic refcount — which is
a very different trade-off once Stage 8 makes the cache multi-threaded.

### No hidden insertion

`get` and `contains` use `find`, never `operator[]`. `operator[]` on an
`unordered_map` **default-constructs a value when the key is absent**, so a
lookup miss would silently insert an entry. For a cache that would mean a stream
of misses growing memory without bound. Two tests pin this
(`get_missing_key_returns_nullopt`, `contains_does_not_insert_on_a_miss`).

---

## 7. Benchmark Methodology

Source: `benchmarks/cache_benchmark.cpp`. Results: `benchmarks/RESULTS.md`.

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DCACHEX_BUILD_BENCHMARKS=ON
cmake --build build/release -j
./build/release/bin/cachex_bench
```

### What is measured

Six phases, each performing exactly 200,000 operations so they share a divisor:
`SET insert`, `SET update`, `GET hit`, `GET miss`, `CONTAINS`, `ERASE`. For each:
total wall time, operations per second, and average latency (total ÷ operations).

### Design decisions, and why each one is there

| Decision | Reason |
| --- | --- |
| `std::chrono::steady_clock` | Monotonic. `system_clock` can jump backwards when NTP adjusts it, which produces negative durations. `high_resolution_clock` is an alias for one of the two and which one is implementation-defined, so it is not safe to rely on. |
| Time a whole phase, then divide | A `steady_clock` read costs ~20–25 ns — comparable to the operations being measured. Timing each call individually would measure the clock as much as the cache. The cost is that only averages are available; percentiles need a different technique (⬜ Stage 5). |
| Keys built before timing starts | Key construction allocates. Measuring it would measure `std::string`, not the cache. |
| Fixed seed (42), fixed sizes | Two runs, and two *versions*, execute the identical operation sequence. Without this, results are not comparable across versions — the whole point of a baseline. |
| Shuffled access order for reads | Walking keys in insertion order is unrealistically friendly to the CPU cache and the prefetcher, and overstates `GET` throughput. |
| One discarded warm-up run | The first run pays for page faults and a cold CPU cache. |
| 5 repeats, **median** reported | One unlucky run — a scheduler preemption, a background process — skews a mean badly and a median not at all. |
| Results accumulated into a printed checksum | Nothing observes the return value of a `get` otherwise, so `-O3` is entitled to delete the calls being measured. The checksum is printed, which forces the work to happen. It is also identical across runs, which is independent evidence that the workload is deterministic. |
| Fixed-width 16-byte keys | Variable-length keys make hashing and comparison cost drift during a run. 16 bytes also fits small-string storage, so key handling costs no allocation — isolating the cache's cost from `std::string`'s. |
| Warns if `NDEBUG` is undefined | Benchmarking a Debug build measures the absence of the optimiser. The binary says so rather than letting someone quietly publish the wrong number. |
| Build config stamped into the output | A saved result sheet always records which compiler and configuration produced it. |

### What is deliberately not measured yet

- **Latency percentiles (p50/p99).** ⬜ Stage 5. For a cache the tail matters far more than the average, but doing it honestly needs per-operation timing without per-operation clock overhead.
- **Hit rate.** Meaningless today: the cache is unbounded, so a key is present iff it was set. It becomes the headline metric in ⬜ Stage 3, once eviction exists.
- **Skewed key distributions.** Real traffic is Zipfian. Uniform access is the pessimistic case for a cache and the honest starting baseline. ⬜ Stage 3.
- **Concurrency scaling.** ⬜ Stage 8.
- **Memory footprint per entry.** ⬜ Stage 11.

### What the harness does not control, and why it matters

The one thing this harness cannot do is control the machine it runs on, and on a
laptop that turns out to dominate everything else. Across 12 invocations of the
identical binary, the three memory-latency-bound phases (`SET update`, `GET hit`,
`ERASE`) fell into two clusters **~2× apart**, while the three hash-lookup-bound
phases varied by only ~20%. The cause was not established; CPU contention alone
does not explain it, and performance-core vs. efficiency-core scheduling is the
leading hypothesis. Details and evidence are in `benchmarks/RESULTS.md`.

The consequence is a rule for every later stage:

> **Absolute throughput numbers are only comparable within a single sitting.
> Ratios between phases in the same run are the trustworthy signal.**

An optimisation in Stage 11 must therefore be demonstrated by running the old and
new versions back to back, interleaved — not by comparing against a number
recorded weeks earlier. Getting a measurement environment that does not need this
caveat is part of ⬜ Stage 5.

### Reading the results

The numbers are in `benchmarks/RESULTS.md` with hardware, variance, and
interpretation. Three conclusions from the Stage 2 baseline are worth carrying
forward, and all three held in *both* performance clusters:

- `CONTAINS` ≈ `GET miss` — this is the cost of the hash lookup alone.
- `GET hit` costs ~6× `CONTAINS`, and the difference is the splice plus the
  64-byte value copy. That is the measured price of returning a copy (§6).
- `SET update` costs ~3.5× `SET insert` despite doing strictly less work, purely
  because of memory access patterns.

All are constant-factor effects on operations that are all O(1).

---

## 8. Current Components

| Path | Purpose | Status |
| --- | --- | --- |
| `CMakeLists.txt` | Language standard, build options, warning flags | ✅ |
| `include/cachex/cache.hpp` | Public `Cache` API | ✅ |
| `include/cachex/recency_list.hpp` | `Entry` and `RecencyList` | ✅ |
| `src/cache.cpp`, `src/recency_list.cpp` | Implementations | ✅ |
| `src/main.cpp` | Short in-process demo; ⬜ becomes the server in Stage 6 | ✅ |
| `tests/` | 48 tests: framework, cache semantics, recency ordering, version | ✅ |
| `benchmarks/` | `cachex_bench` + `RESULTS.md`; off by default | ✅ |
| `docs/` | Longer-form notes | — |

### Build targets

```
cachex_warnings  (INTERFACE)  warning flags, carried as a target not global flags
        |
        +--> cachex_core  (static library)  cache · recency_list · version
                    |
                    +--> cachex        thin main(), in-process demo
                    +--> cachex_tests  links the same library
                    +--> cachex_bench  links the same library  (opt-in)
```

**The library/executable split is the most important structural decision.** A
`main()` cannot be linked into a test binary — there would be two. If the logic
lived in `main.cpp` it would be untestable, and the usual workaround (tests
`#include`-ing the `.cpp`, or recompiling the sources) lets the two copies drift.
`cachex_core` means tests and benchmarks exercise *exactly* the object code the
server will run.

---

## 9. Design Decisions

### 9.1 Why C++17 rather than C++20

C++17 already contains everything this project needs, and Stage 2 uses three of
them directly: `std::optional` for lookups that may miss, structured bindings and
`if`-with-initialiser for the map lookups in `set`/`get`/`erase`, and
`std::shared_mutex` waiting for Stage 8. It is also available everywhere without
effort — Apple Clang, GCC 9+, MSVC 2019 — so anyone cloning this builds it with
the compiler already on their machine.

| C++20 feature | Why it is not needed |
| --- | --- |
| Coroutines | The one genuine temptation, for async networking. But an explicit `epoll`/`kqueue` loop makes the I/O model *visible*, and being able to explain readiness-based I/O is worth more than hiding it behind `co_await`. |
| Concepts / ranges | Real improvements to generic code. CacheX has almost no generic code. |
| `std::format` | Convenience only; library support is still uneven. |
| Heterogeneous lookup with `string_view` | ⚠️ The one real loss. `unordered_map<string, T>::find(string_view)` needs C++20's transparent hashing; in C++17 a lookup from a `string_view` must construct a temporary `std::string`. It does not bite yet — `Cache::get` takes `const std::string&` — but it will when the protocol parser hands over views into a socket buffer (Stage 7). The fix is a transparent hash functor, not a language upgrade. |

The honest trade-off: C++17 costs us `std::span` and the heterogeneous lookup
above. The deciding factor is that **every C++17 feature used here can be
explained in one sentence**, which is the bar this project sets.

### 9.2 Why CMake

The de-facto standard for C++, so it is what reviewers expect and what IDEs,
`clangd`, sanitizers, and CI already understand. It makes Debug/Release a
configuration flag rather than hand-maintained compiler invocations.

The CMake here is **target-based**: properties attach to targets
(`target_include_directories`, `target_link_libraries`) rather than directory-wide
globals, so include paths and flags travel with the target that needs them.

### 9.3 Why warnings are an INTERFACE target

`cachex_warnings` carries no code, only flags; targets opt in by linking it.
Appending to global `CMAKE_CXX_FLAGS` would apply them to any third-party library
added later, whose warnings are not ours to fix — and hundreds of them scrolling
past is how a real warning in our own code gets missed.

`-Werror` is available (`-DCACHEX_WARNINGS_AS_ERRORS=ON`) but off by default, so a
new compiler version with a new warning cannot break someone's clone.

`-Wconversion`/`-Wsign-conversion` earn their place in a project full of `size_t`
counts, and `-Wshadow` catches a local hiding a member.

### 9.4 Why the test framework is hand-written

~100 lines of header, no dependency. Only three capabilities are needed: register
a test, assert a condition, exit non-zero. GoogleTest or Catch2 would be the
largest thing in the repository and would add a fetch step between clone and
build. The macro names (`CACHEX_TEST`, `CHECK`, `CHECK_EQ`) mirror Catch2's, so
swapping later is contained.

One non-obvious detail: the test registry is a **function-local static**, not a
namespace-scope global. Tests in different translation units register themselves
before `main()`, and cross-translation-unit initialisation order is unspecified —
a namespace-scope registry could be registered *into* before it was constructed.
A function-local static is constructed on first use, which removes the problem.
This is the *static initialisation order fiasco*.

### 9.5 Why the tests run under sanitizers

The correctness of this design rests on a claim about iterator validity, and
nothing in the type system enforces it. A dangling `std::list` iterator will
usually still *appear* to work, because the freed node's memory is typically
untouched. Tests alone would not catch it.

```bash
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build/asan -j --target cachex_tests && ./build/asan/bin/cachex_tests
```

### 9.6 Why the version header is generated

`configure_file()` expands `include/cachex/version.hpp.in` into the build
directory, substituting the version from `project()`. Otherwise the version lives
in two places and drifts. The generated header goes in the build tree, never the
source tree.

### 9.7 Why `build/` is not committed

Build output is reproducible from the sources, specific to one compiler and one
machine, goes stale the instant a flag changes, and makes every diff unreadable.
Commit the inputs, never the outputs.

---

## 10. Future Roadmap

**Everything below is ⬜ future work.**

| # | Stage | What it adds | Core concepts | Status |
| --- | --- | --- | --- | --- |
| 1 | Project foundation | CMake, structure, warnings, test harness | Build systems, testability | ✅ |
| 2 | **Core cache** | `set`/`get`/`erase`/`contains`/`size`, hash map + list | Hash tables, iterator invalidation, ownership | ✅ |
| 3 | LRU eviction | Capacity limit; evict from `oldest()` | Why O(1) LRU needs both structures; hit rate | ⬜ |
| 4 | TTL | Per-key expiry deadlines on `Entry` | Lazy vs. active expiry; `steady_clock` vs. `system_clock` | ⬜ |
| 5 | Benchmark harness II | p50/p99 latency, hit rate, skewed keys | Why the average latency lies | ⬜ |
| 6 | TCP server | `socket`/`bind`/`listen`/`accept`, one client | The socket API; blocking I/O; partial reads | ⬜ |
| 7 | Client protocol | Line-based text protocol and parser | Framing, buffering, malformed input | ⬜ |
| 8 | Concurrency | Many clients — thread pool or event loop | Data races, `shared_mutex`, deadlock avoidance | ⬜ |
| 9 | Sharded cache | N independently locked shards | Lock contention as the real bottleneck | ⬜ |
| 10 | Persistence | Snapshot to disk, restore on startup | Serialisation; durability vs. throughput | ⬜ |
| 11 | Final optimisation | Profile, tune, re-benchmark against the Stage 2 baseline | Cache locality, allocation cost, proving an improvement | ⬜ |

Each stage ends with this document updated: components in §8, decisions in §9,
and the diagram in §3 grown to match what actually exists.
