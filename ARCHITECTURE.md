# CacheX — Architecture

> Living document. It is updated at every stage of the project.
> **Current stage: 9 — benchmark suite, profiling and a measured optimisation.**

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
| Eviction | Fixed capacity with an LRU policy, O(1) per operation | ✅ Stage 3 |
| Expiry | Per-key TTL, lazy expiration | ✅ Stage 4 |
| Measurement | Benchmark harness: throughput, average latency, percentiles, hit rate | ✅ Stage 3–4 |
| Benchmark suite | Standard workloads, three versions, profiling, committed raw results | ✅ Stage 9 |
| Active expiry | Background sweep of expired keys | ⬜ Stage 10 |
| Event-loop I/O | Many connections served by a few threads instead of one thread each | ⬜ next — the measured bottleneck |
| Networking | Single-node TCP server, line-based text protocol | ✅ Stage 5 |
| Concurrency | Multiple clients served safely and, where possible, in parallel | ✅ Stage 6 |
| Length-prefixed framing | Keys/values containing whitespace or binary data | ⬜ future |
| Sharding | Cache split into independently locked shards | ✅ Stage 7 |
| Persistence | Snapshot/restore so state survives a restart | ✅ Stage 8 |

### Explicitly out of scope

Excluded on purpose, so the project stays finishable and explainable. Each is a
good interview answer in its own right — *"here is why I did not build it"*.

- **Clustering, replication, failover.** Distributed consensus is a project of its own; single-node already exercises the data structures and concurrency.
- **Rich data types** (lists, sets, sorted sets, hashes). Surface area, not insight — the interesting problems are eviction, expiry, and concurrency.
- **Authentication, TLS, multi-tenancy.** CacheX assumes a trusted local network, exactly as Redis does by default. It should not be exposed to the public internet.
- **Lua scripting, pub/sub, transactions.** Orthogonal to the caching problem.
- **Hand-written lock-free data structures.** Enormous difficulty-to-benefit ratio, and nearly impossible to defend confidently under pressure. Sharded locking gets most of the scalability at a fraction of the risk.
- **Approximated LRU.** Redis samples a few random keys and evicts the oldest of the sample, trading exactness for a smaller per-entry footprint. CacheX implements *exact* LRU because the point here is to demonstrate that the O(1) structure works, not to save 16 bytes per key.

---

## 3. High-Level Architecture

```
   cachex_client / netcat / any TCP client
                  |
                  |  TCP, line-based text protocol (docs/PROTOCOL.md)
                  v
   +------------------------------------------+
   |  Server        accept loop, one thread   |  server.cpp
   +------------------------------------------+
        |  one worker thread per connection (max 256)
        v
   +------------------------------------------+
   |  Connection    recv loop, LineBuffer     |  connection.cpp, line_buffer.cpp
   |  parse_command -> execute -> reply_*     |  protocol.cpp, command_handler.cpp
   +------------------------------------------+
        |                                  |
        |  GET SET DELETE EXISTS TTL       |  SAVE / LOAD
        v                                  v
===========================================|====== nothing below knows about sockets
   +--------------------------------+   +----------------------------+
   |  ShardedCache  (8 by default)  |<--|  PersistenceManager        |
   |  shard = splitmix(hash(key))%N |   |  export_entries() -> file  |
   +--------------------------------+   |  file -> set()             |
        |        |    ...    |          |  PeriodicSaver (optional)  |
        v        v           v          +----------------------------+
   +---------+ +---------+ +---------+               |
   |SyncCache| |SyncCache| |SyncCache|               v
   | mutex   | | mutex   | | mutex   |      snapshot file on disk
   | Cache   | | Cache   | | Cache   |      (temp file + atomic rename)
   +---------+ +---------+ +---------+
        |
        v   each Cache:
   +----------------------+   +------------------------------+
   |     RecencyList      |<--|            index_            |
   |   std::list<Entry>   |   |  unordered_map<string,       |
   |  head (MRU) ... tail |   |    RecencyList::Iterator>    |
   +----------------------+   +------------------------------+
     LRU eviction at tail       O(1) lookup
     Entry::expires_at checked lazily on access

   not built yet:  active expiry sweep (Stage 10) · event-loop I/O
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
        (newest)                  (oldest = next to be evicted)
```

Each map value is a `std::list` iterator pointing directly at that key's node.
A lookup is one hash (O(1)); promoting the node it found to the head is a splice
(O(1)); the eviction candidate is always the tail (O(1)). Neither structure can
do the other's job — a hash map has no ordering, and a linked list cannot search
— so the cache is the pair of them.

---

## 4. Cache Design

### 4.1 Class layout

Two classes, deliberately not one:

```cpp
struct Entry {                  // recency_list.hpp
  std::string key;
  std::string value;
  std::optional<Clock::time_point> expires_at{};   // nullopt = never expires
};

class RecencyList {             // owns the data, maintains MRU -> LRU order
  std::list<Entry> entries_;
 public:
  Iterator insert_newest(std::string key, std::string value);
  void mark_used(Iterator it);  // splice to head
  void erase(Iterator it);
  const Entry& oldest() const;  // the eviction candidate
  void pop_oldest();
};

class Cache {                   // the public API; owns the index
  RecencyList entries_;
  std::unordered_map<std::string, RecencyList::Iterator> index_;
  std::optional<std::size_t> capacity_;   // nullopt = unbounded
  std::size_t evictions_ = 0;
};
```

**Why split them at all?** `RecencyList` is the *eviction policy*, and it is the
part most likely to change — a plausible later variant is LFU or segmented LRU.
Keeping it behind six named operations means `Cache` never touches `std::list`
directly, so swapping the policy touches one file. It also makes the ordering
invariant testable on its own (`tests/recency_list_test.cpp`), separately from
cache semantics.

It is a thin class on purpose. It is **not** an abstract base class, there is no
policy template, and there is no factory — none of that is needed to swap one
concrete implementation for another at this size, and all of it would cost more
in indirection than it returns.

### 4.2 The algorithms

| Operation | What actually happens |
| --- | --- |
| `set` (new key) | Hash → miss. Push a new node at the head. Insert `key → iterator` into the map. Trim to capacity (§5.3). |
| `set` with a TTL | As above, but `expires_at = now + ttl`. A `ttl <= 0` erases instead of storing (§6.1). |
| `ttl` | Hash → reports Missing / Persistent / Expiring. Reclaims the entry if expired. Not a use. |
| `set` (existing) | Hash → hit. Move-assign the new value into the existing node. Splice the node to the head. **The node is never destroyed, so the map's iterator stays valid.** No eviction: the entry count is unchanged. |
| `get` (hit) | Hash → hit. **If expired, reclaim it and report a miss.** Otherwise splice the node to the head and return a copy of the value. |
| `get` (miss) | Hash → miss. Return `std::nullopt`. Nothing inserted, nothing reordered. |
| `erase` | Hash → hit. Erase the list node **first** (the map entry is what tells us which node), then erase the map entry. Frees a slot, so the next insert need not evict. |
| `contains` | Hash → hit or miss. Deliberately does *not* reorder — see §4.6. |
| `size` | `index_.size()`. Never exceeds `capacity()`. Includes expired entries not yet reclaimed (§6.4). |

### 4.3 Why `std::unordered_map`

The cache's defining operation is "given a key, find its value, fast". That is
exactly a hash table: average O(1), independent of how many keys are stored.

- **vs. `std::map` (red-black tree):** O(log n) instead of O(1), and it would demand that keys be *ordered*, which a cache has no use for. At 1,000,000 keys that is ~20 full string comparisons versus one hash. The only thing `std::map` would buy is range queries, which CacheX does not have.
- **vs. `std::vector` + linear scan:** O(n). Fine for ten keys, unusable at a hundred thousand.
- **vs. a hand-written hash table:** the interesting part of this project is the *combination* of structures and the eviction policy, not re-implementing open addressing. `std::unordered_map` is well-tested and everyone reading the code already knows its semantics. Stage 11 can revisit this with measurements in hand — its node-per-element layout is genuinely cache-unfriendly, and that is a real, *measurable* argument to make later rather than a guess to make now.

**Its weaknesses, stated honestly** — these are the follow-up questions:

- O(1) is *average*, not worst case. Adversarial keys that all hash to one bucket degrade it to O(n). Real caches on untrusted input mitigate this with a randomly seeded hash; CacheX assumes a trusted network (§2), so it does not.
- It is node-based: every element is a separate allocation, and traversal chases pointers. This shows up clearly in the benchmark (§13).
- Growth rehashes every element, so an individual `set` can be O(n) even though the amortised cost is O(1). A *bounded* cache reaches its final bucket count and then stops rehashing, so in the steady state this stops happening at all — one of the quieter benefits of adding capacity.

### 4.4 Why a doubly linked list

The list answers a question the hash map cannot: **which entry has gone longest
without being used?** It is the structure that makes O(1) eviction possible.

Three properties matter, and `std::list` is close to the only standard container
with all three:

1. **O(1) splice.** `entries_.splice(begin, entries_, it)` moves a node to the front by rewriting a handful of pointers. Nothing is copied, nothing is allocated, and no other element moves. This is the single operation the entire LRU design is built on.
2. **Iterator and reference stability.** Inserting or erasing any element leaves iterators to *every other* element valid. This is what lets the hash map store an iterator per key and trust it indefinitely. `std::vector` fails here outright — one reallocation and every stored iterator dangles.
3. **O(1) access to both ends.** The head is the most recently used; the tail is the least. Eviction is `pop_oldest()`.

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
alternatives — a sentinel value, or `bool get(key, string& out)` — either
constrain what can be stored or make every call site two statements.

`capacity()` returns `std::optional<std::size_t>`: `nullopt` means unbounded.
A sentinel like `0` or `SIZE_MAX` would have to mean either "store nothing" or
"store everything", and both readings are defensible — which is exactly why a
sentinel is the wrong tool. `optional` makes "no limit" a distinct state from any
numeric limit, so `Cache(0)` unambiguously means *capacity zero*, and it is
tested as such.

### 4.6 Recency semantics: what counts as a "use"

LRU only means something once "recently used" is pinned down. CacheX follows the
conventional cache semantics, and the choices are tested rather than implied:

| Operation | Counts as a use? | Reasoning |
| --- | --- | --- |
| `get` **hit** | ✅ yes | The definition of LRU. |
| `get` **miss** | ➖ n/a | There is no entry to reorder. Nothing is inserted either. |
| `set` **insert** | ✅ yes | A new entry starts as the most recently used, so it is never its own eviction victim. |
| `set` **update** | ✅ **yes** | This is the one that needs a decision. Writing to a key is at least as strong a signal of interest as reading it, so an update moves the entry to the front — matching Redis, memcached, and the standard LRU-cache formulation. The alternative (a write refreshes the value but not the position) would let a key that is written constantly and read never be evicted while hot, which is surprising. |
| `erase` | ➖ n/a | The entry is gone. It frees a slot, so the next insert need not evict. |
| `ttl` | ❌ **no** | A metadata query should not rescue a key from eviction, for the same reason `contains` should not. |
| `contains` | ❌ **no** | Deliberate. Peeking is not using: `EXISTS` must not become a way to keep dead data alive forever. This is also what lets `contains` be `const` while `get` cannot be — a consequence worth noticing rather than fighting. |

---

## 5. The LRU Algorithm

### 5.1 Why a hash map plus a doubly linked list gives O(1)

LRU needs three things, and no single container does all three:

| Requirement | Hash map alone | Linked list alone | The pair |
| --- | --- | --- | --- |
| Find a key's value | ✅ O(1) | ❌ O(n) — must walk | ✅ O(1) |
| Know which entry is oldest | ❌ no ordering at all | ✅ O(1) — it is the tail | ✅ O(1) |
| Mark an entry as just-used | ❌ nothing to update | ⚠️ O(1) *only if you already hold the node* | ✅ O(1) |

The last row is the crux. A linked list can move a node to the front in O(1) —
but only if you already have a handle to it. Finding that node by key would be an
O(n) walk, which is what makes a list alone useless.

**The bridge is that the map's values are iterators into the list.** The list is
never searched; it is only ever mutated through a handle the map just produced.
So every operation decomposes into three O(1) steps:

```
  1. find the node           index_.find(key)        O(1) average
  2. move or remove it       splice / erase / pop    O(1) worst case
  3. keep the index in sync  index_.erase / emplace  O(1) average
```

This only works because **`std::list` iterators stay valid**. Inserting or
erasing any element leaves handles to every other element intact, so an iterator
stored in the map remains correct for the entire life of that entry, across any
number of unrelated operations. Swap in a `std::vector` and the first
reallocation turns every stored handle into a dangling pointer.

The "average" qualifier attaches only to the hash map. The list operations are
O(1) in the worst case; it is bucket collisions — never the LRU logic — that can
degrade an operation to O(n).

### 5.2 Recency update flow — what happens on a GET hit

```
  cache.get("b")   with the cache holding   head [d] <-> [c] <-> [b] <-> [a] tail

  1. index_.find("b")            O(1) avg    ->  iterator to node [b]
  2. entries_.mark_used(it)      O(1)        ->  splice [b] to the head
  3. copy the value out          O(value)    ->  std::optional<std::string>

  result:                                  head [b] <-> [d] <-> [c] <-> [a] tail
                                                                         ^
                                                        "a" is now the eviction
                                                        candidate instead of "b"
```

Step 2 is a `std::list::splice` of one node within its own list: three pointer
pairs rewritten, no allocation, no element copied, and the iterator remains
valid. The standard defines it as a no-op when the node is already at the
requested position, so a `get` on the newest key needs no special case — which
`tests/recency_list_test.cpp: mark_used_on_the_front_entry_is_a_no_op` confirms.

A `get` **miss** does none of this: it returns `nullopt` and leaves the order
untouched, so a flood of lookups for keys that do not exist cannot disturb the
eviction order (`a_get_miss_does_not_disturb_the_eviction_order`).

### 5.3 Eviction flow — what happens when capacity is reached

```
  capacity = 4, cache is full:             head [d] <-> [c] <-> [b] <-> [a] tail

  cache.set("e", v)

  1. index_.find("e")               O(1) avg   ->  miss, so this is an insert
  2. entries_.insert_newest(...)    O(1)       ->  head [e] <-> [d] <-> [c] <-> [b] <-> [a]
     index_.emplace("e", node)      O(1) avg       size is now 5
  3. size (5) > capacity (4)  ->  evict:
       a. entries_.oldest()         O(1)       ->  node [a], the tail
       b. read victim.key           O(1)       ->  "a"      <-- why Entry stores the key
       c. index_.erase("a")         O(1) avg   ->  drop the index entry
       d. entries_.pop_oldest()     O(1)       ->  destroy the node
     ++evictions_

  result:                                  head [e] <-> [d] <-> [c] <-> [b] tail
```

Three details in that sequence carry real weight:

**Step 3b is why `Entry` stores its own key.** Eviction runs *list → map*: the
list knows which entry is oldest, but removing it from the index requires its
key. Without the key in the node, the only way to find the index entry would be
to scan the entire map — O(n), which would defeat the whole design. The cost is
one duplicated key per entry, and it buys the O(1) bound.

**The ordering in 3c → 3d is not interchangeable.** `victim.key` is a reference
*into the list node*. Erasing from the map does not touch the list, so the
reference stays valid until `pop_oldest()` destroys the node. Pop first and the
subsequent `index_.erase` reads freed memory.

**Insert first, then trim.** The alternative — evict first, then insert — would
avoid momentarily holding `capacity + 1` entries. Insert-then-trim was chosen
because it is one code path rather than two, and because the new entry is at the
head and therefore never its own victim. The cost is a transient single extra
entry. The one case where the new entry *is* evicted is `capacity == 0`, which
falls out of the same code with no special case: insert, then immediately trim it
away, leaving the cache empty (`capacity_zero_stores_nothing`).

The trim is a `while` rather than an `if`. In the steady state it runs at most
once, so `set` stays O(1); the loop exists so that `capacity == 0` terminates
correctly and so a future "lower the capacity at runtime" operation would not
silently leave the cache over its limit.
---

## 6. TTL and Expiration

### 6.1 The model

A TTL is attached at write time and is a property of the entry, not of a separate
index:

```cpp
struct Entry {
  using Clock = std::chrono::steady_clock;
  std::string key;
  std::string value;
  std::optional<Clock::time_point> expires_at{};   // nullopt = never expires
};
```

```cpp
cache.set("k", "v");                    // persistent
cache.set("k", "v", 30s);               // expires 30 s from now
cache.ttl("k");                         // TtlInfo{ Expiring, ~30000ms }
```

**An absolute deadline, not a remaining duration.** A duration would have to be
re-based against "when was this set", so the entry would need to store *both* a
start time and a length, and every check would be a subtraction before the
comparison. One `time_point` answers "is this expired?" with a single `<=`.

**`std::optional`, not a sentinel.** "Never expires" is a genuinely different
state from any deadline, not a magic value of one. It also buys the performance
property that matters most: `has_value()` is checked *first*, so an entry without
a TTL never reads the clock at all. That short-circuit is why TTL is nearly free
for keys that do not use it — and §13 measures exactly how much it costs for keys
that do.

`ttl()` returns a small tagged type rather than an integer:

```cpp
enum class TtlState { Missing, Persistent, Expiring };
struct TtlInfo { TtlState state; std::chrono::milliseconds remaining; };
```

Redis encodes the same three states in one integer using two magic values
(`-2` missing, `-1` no expiry, `>= 0` seconds remaining). The enum makes the
three cases impossible to confuse — you cannot accidentally do arithmetic on
"missing".

### Defined behaviour

| Case | Behaviour | Why |
| --- | --- | --- |
| `set(k, v)` on a key that **had** a TTL | The TTL is **cleared**; the key becomes persistent | A set replaces the whole entry, expiry included. This is Redis's `SET` default; Redis needs an explicit `KEEPTTL` flag to do otherwise. One rule, no hidden state carried across a write. |
| `set(k, v, ttl)` on an existing key | The old deadline is replaced by the new one | Same rule: the write defines the entry completely. Extending *and* shortening both work. |
| `set(k, v, ttl)` with **`ttl <= 0`** | The key is **erased** and nothing is stored | The entry could never be read, and storing it would be pure cost — it would occupy capacity and could evict a live entry. The invariant: *after `set(k, v, ttl)`, the key is visible iff `ttl > 0`.* Note Redis instead rejects `SET ... EX 0` as an error; CacheX chooses the delete semantics of `EXPIRE key 0`. |
| `set(k, v, ttl)` with `ttl <= 0` on a **missing** key | No-op | Nothing to erase. |
| `ttl()` on a missing key | `Missing` | |
| `ttl()` on an expired key | `Missing`, and the entry is reclaimed | An expired key is indistinguishable from an absent one. |
| `get()` on an expired key | `nullopt`, and the entry is reclaimed | |
| `contains()` on an expired key | `false`, entry **not** reclaimed | The one deliberately non-mutating peek — see §6.4. |
| `erase()` on an expired key | Returns **`false`**, entry is reclaimed | Nothing user-visible was removed: the key was already gone. |
| `size()` with expired entries present | **Counts them** | It reports entries *resident*, not entries *visible*. See §6.4. |

### 6.2 Why a monotonic clock

`Entry::Clock` is `std::chrono::steady_clock`, never `system_clock`.

`system_clock` tracks wall-clock time and **can jump**: NTP corrections, an
administrator setting the date, a VM resuming from a snapshot, DST handling bugs.
A deadline stored against a clock that jumps produces exactly the wrong
behaviour in both directions:

- **Clock jumps backwards** → every deadline is suddenly further away. Keys that should have expired stay alive, serving stale data for as long as the jump.
- **Clock jumps forwards** → every deadline is suddenly in the past. The entire cache mass-expires at once, and the next traffic burst goes straight to the database. That is a cache stampede caused by nothing but a clock adjustment.

`steady_clock` is monotonic by definition: it never goes backwards and its rate is
not adjusted. It measures elapsed time, which is precisely what a TTL is.

**The cost of that choice, stated honestly:** `steady_clock`'s epoch is
unspecified — in practice it is usually time since boot. A `steady_clock`
`time_point` is therefore **meaningless outside the running process** and cannot
be serialised. When persistence arrived in Stage 8 this had to be dealt with:
snapshots store each entry's *remaining duration* plus a wall-clock timestamp of
the save, and the loader subtracts the time the file spent on disk (§10.2). That
is the right trade — correctness while running matters more than convenience
while saving — but it was a real consequence, not a free lunch.

### 6.3 Lazy expiration

CacheX expires keys **lazily**: nothing runs in the background, and an expired
entry is removed at the moment some operation happens to encounter it.

```cpp
std::optional<std::string> Cache::get(const std::string& key) {
  const auto it = index_.find(key);
  if (it == index_.end()) return std::nullopt;

  if (is_expired(*it->second)) {     // we are already holding the entry...
    remove(it);                      // ...so reclaim it here and now
    ++expired_removals_;
    return std::nullopt;
  }
  entries_.mark_used(it->second);
  return it->second->value;
}
```

```cpp
bool is_expired(const Entry& entry) {
  // has_value() first: an entry with no deadline never reads the clock.
  return entry.expires_at.has_value() && *entry.expires_at <= Entry::Clock::now();
}
```

The appeal is that the work is already paid for. The lookup has found the entry
and the iterator is in hand, so removing it costs the same O(1) teardown as any
other removal — no search, no separate index of deadlines, no coordination.

**Which operations reclaim, and which do not:**

| Operation | Reports expiry | Reclaims | |
| --- | --- | --- | --- |
| `get` | ✅ | ✅ | Mutates anyway |
| `ttl` | ✅ | ✅ | Mutates anyway |
| `erase` | ✅ (returns `false`) | ✅ | Mutates anyway |
| `contains` | ✅ | ❌ | `const`; see below |

`contains` is the deliberate exception. It has been `const` since Stage 2 —
it is the non-mutating peek that also does not count as a use — and reclaiming
would break that. So it tells the truth about expiry and leaves the body for the
next mutating call to clear. The alternative was to make `contains` non-`const`,
which would mean *every* read path mutates and there is no way to ask a question
of the cache without changing it. Pinned by
`tests/ttl_test.cpp: contains_reports_expiry_without_reclaiming`.

### 6.4 What lazy expiration costs

These are real limitations, not hypotheticals, and each has a test that pins the
behaviour so it cannot drift silently.

**1. Expired entries occupy memory until something touches them.** A key with a
1-second TTL that is never read again sits in the cache forever. In the worst
case — a large write-once workload where most keys are never re-read — expired
data accumulates without bound. This is the single biggest reason a real cache
eventually needs active expiry.

**2. Expired entries occupy *capacity*, so they can evict live data.** This is
worse than the memory cost. A bounded cache full of expired corpses will evict a
live, frequently used entry to make room for a new one, because eviction picks
the LRU tail and has no idea that some entries are already dead.
(`expired_entries_still_occupy_capacity_until_reclaimed`.)

**3. `size()` is an upper bound, not a count.** It reports entries resident,
including expired ones. Reporting only live keys would mean scanning — O(n) —
which is not something a `size()` call should do. Redis's `DBSIZE` has exactly
the same property, for the same reason.

**4. Reclamation is unbounded in when, not in how much.** No single operation
does more than O(1) of expiry work, so there is no latency spike — but there is
also no guarantee that expired memory is ever returned.

**5. Memory is not returned in bulk.** Even once entries are reclaimed one at a
time, the allocator may not return pages to the OS.

### 6.5 Why there is no background cleanup thread yet

A sweeper thread is the obvious fix for every limitation above. It is
deliberately deferred, for reasons in this order:

1. **The cache was not thread-safe when TTL was added.** A background thread
   mutating the map and the list while a caller held an iterator into them would
   have been a data race and an almost-guaranteed use-after-free. Adding a thread
   then would have meant inventing the concurrency design as a side effect of a
   feature about time. *This blocker is now gone: locking arrived in Stage 6 and
   sharding in Stage 7.*

2. **It would make the design harder to explain, for no gain yet.** Lazy
   expiration is ~6 lines and the whole mechanism fits in one paragraph. A
   sweeper adds a thread lifecycle, a shutdown path, a sampling policy, and a
   tuning knob for how aggressive to be.

3. **The correct design needs measurements that do not exist yet.** Redis samples
   20 random keys per cycle and repeats while more than 25% of the sample was
   expired — those constants are the product of production tuning, not first
   principles. Picking numbers now would be guessing.

4. **Sampling needs a data structure the cache does not have.** Picking a random
   key from an `unordered_map` is not O(1), and neither structure here supports
   efficient random sampling. Active expiry would need either a separate index of
   deadlines (a priority queue or a bucketed timer wheel) or a way to sample the
   map cheaply. That is a design decision of its own.

Where this stands now: reason 1 is resolved (Stage 6), and reasons 3 and 4 are
what remain. ⬜ Active expiry is Stage 10: a per-shard sweep under that shard's
lock, with the sampling rate chosen from measurements of how much expired memory
actually accumulates under the `ttl-heavy` benchmark workload.

### 6.6 What Redis Does Differently

**CacheX's TTL is intentionally simplified and does not behave like Redis.** It
implements the idea, not the product. The differences worth knowing:

| | CacheX | Redis |
| --- | --- | --- |
| **Expiry strategy** | Lazy only | Lazy **plus** an active cycle that samples 20 random keys ~10×/second and repeats while >25% of the sample was expired |
| **Clock** | `steady_clock` (monotonic) | Wall-clock milliseconds since epoch, so deadlines survive restarts and replicate — at the cost of being sensitive to clock jumps |
| **`TTL` return** | Tagged `TtlInfo` | One integer with magic values: `-2` missing, `-1` no expiry, `>= 0` remaining |
| **`SET` with a zero TTL** | Erases the key | Rejected as an error (`invalid expire time`); `EXPIRE key 0` is what deletes |
| **Changing a TTL** | Only via `set` | Dedicated commands: `EXPIRE`, `PEXPIRE`, `EXPIREAT`, `PERSIST`, `GETEX` — TTL can be changed without rewriting the value |
| **`SET` and existing TTL** | Always cleared | Cleared by default, kept with `KEEPTTL` |
| **Replication** | N/A | The primary decides expiry and sends explicit `DEL` to replicas, so replicas never expire keys on their own — otherwise clock drift would desynchronise them |
| **Persistence** | N/A | Deadlines are written into RDB/AOF and survive restarts |
| **Eviction interaction** | LRU ignores expiry; a dead entry can evict a live one | Eight configurable policies (`volatile-lru`, `allkeys-lru`, `volatile-ttl`, …), some of which target keys with a TTL specifically |
| **LRU itself** | Exact | Approximated by sampling, to avoid the per-entry list pointers |

The gap that matters most is **active expiry**. Everything in §6.4 is a
consequence of not having it, and Redis has it precisely because those problems
are real at production scale.
---

## 7. Client/Server Architecture

### 7.1 Why TCP, and what "protocol" means twice over

**TCP over UDP.** A cache client needs to know that its `SET` arrived, arrived
once, and arrived intact — and a `GET` reply larger than a packet has to be
reassembled in order. TCP provides exactly that: reliable, ordered, de-duplicated
delivery with retransmission and flow control. Over UDP every one of those would
have to be rebuilt by hand, and the result would be a worse TCP. Redis, memcached
and every mainstream database use TCP for the same reason.

UDP is the right choice when *late data is worthless* — live audio, video,
telemetry — where retransmitting a packet that is already too old to use only
makes things worse. A cache is the opposite: a slightly late answer is still the
right answer. (memcached does offer a UDP mode, for `get`-only traffic where a
lost reply can simply be re-fetched. CacheX does not.)

**Two different protocols, stacked.** This distinction is worth being precise
about:

| | Transport protocol (TCP) | Application protocol (CacheX v1) |
| --- | --- | --- |
| Provided by | The operating system's network stack | This codebase, in `protocol.cpp` |
| Guarantees | Bytes arrive, in order, without duplication or corruption | What those bytes *mean* |
| Unit | A byte stream — **no message boundaries at all** | One command per line |
| Knows about | Ports, sequence numbers, windows, retransmission | `SET`, `GET`, `+OK`, `-ERR` |

TCP will faithfully deliver every byte of `SET foo bar` and has no idea that
those bytes form a command. **The application protocol's whole job is to put
boundaries back.** That is why §7.4 exists, and why `recv()` returning half a
command is normal rather than a bug.

### 7.2 Component layout

```
                         Client process
   +------------------------------------------------+
   |  cachex_client  --  REPL, LineBuffer, Socket    |
   +------------------------------------------------+
                          |
                          |  TCP, loopback or LAN
                          v
   ==================== Server process ==============================
   +------------------------------------------------+
   |  Server            socket/bind/listen/accept    |  server.cpp
   +------------------------------------------------+
                          | one accepted Socket
                          v
   +------------------------------------------------+
   |  Connection        recv loop, framing, replies  |  connection.cpp
   +------------------------------------------------+
              |                               ^
              | line                          | reply string
              v                               |
   +----------------------+       +--------------------------+
   |  parse_command()     |       |  reply_ok / reply_value  |  protocol.cpp
   |  bytes -> Command    |       |  reply_nil / reply_error |
   +----------------------+       +--------------------------+
              |                               ^
              +-------------> execute() ------+                command_handler.cpp
                                  |
                                  v
   ------------------------------------------------- the boundary
   +------------------------------------------------+
   |  Cache        set · get · erase · contains      |  cache.cpp
   |               ttl · size · capacity             |
   +------------------------------------------------+
                    |                  |
              RecencyList          unordered_map
```

**The cache does not know that sockets exist**, and that is enforced by the build
graph, not just by discipline: `cachex_core` (cache, recency list) does not link
`cachex_net` (protocol, sockets, server). The dependency runs one way —
networking calls the cache. Anything in `cache.cpp` that reached for a socket
would fail to link.

The payoff is testability. `execute(Cache&, Command)` takes a parsed command and
returns a reply string, so the complete request/response behaviour of the server
can be tested without opening a connection
(`protocol_test.cpp: execute_runs_a_whole_request_response_cycle_without_a_socket`).
The integration tests then only have to prove that the *plumbing* works, not the
semantics.

| Component | File | Knows about |
| --- | --- | --- |
| `LineBuffer` | `net/line_buffer.cpp` | Bytes and newlines. No sockets, no commands. |
| `parse_command` / `reply_*` | `net/protocol.cpp` | The grammar. No sockets, no cache. |
| `execute` | `net/command_handler.cpp` | Commands and the cache. **No sockets.** |
| `Socket` | `net/socket.cpp` | File descriptors and their lifetime. |
| `Connection` | `net/connection.cpp` | One client: recv, framing, dispatch, send. |
| `Server` | `net/server.cpp` | The listening socket and the accept loop. |

### 7.3 The TCP request lifecycle

What the four socket calls actually do — they are routinely confused:

```
  SERVER                                          CLIENT

  socket()   create an endpoint. Just a file
             descriptor; no address yet.
      |
  bind()     claim 127.0.0.1:6379. Now the
             address is ours.
      |
  listen()   mark the socket PASSIVE. Does not
             block and accepts nothing. It tells
             the kernel: complete TCP handshakes
             on my behalf and queue the finished
             connections, up to `backlog` deep.
      |
      |                                      socket()
      |                                          |
      |   <====== SYN ========================  connect()  blocks until the
      |   ======= SYN-ACK =================>     |         handshake completes
      |   <====== ACK ========================   |
      |                                          |
      |   The kernel completed that handshake    |
      |   by itself and put the connection on    |
      |   the accept queue. The application      |
      |   has not run any code yet.              |
      |                                          |
  accept()   take one finished connection off
             the queue. Returns a NEW socket for
             that client; the listening socket
             stays open and keeps accepting.
             Two sockets, two jobs.
      |                                          |
      |   <====== "GET foo\n" ================  send()
  recv()     returns whatever bytes have arrived
      |
      |   parse -> execute -> format
      |
  send()  ===== "=bar\n" ==================>   recv()
      |                                          |
      |   ... repeat for the life of the connection ...
      |
      |   <====== FIN ========================  close()
  recv()     returns 0 = end of stream
  close()
```

Three things this makes concrete:

- **`listen()` does not accept and does not block.** It is a one-time state change on the socket. Every subsequent handshake is completed by the kernel whether or not the application ever calls `accept()`.
- **A client's `connect()` can succeed while the server is busy.** The handshake is the kernel's work; the connection then waits in the backlog. So "connected" does not mean "being served".
- **`accept()` returns a different socket.** The listening socket is never read from or written to; it exists only to produce new sockets.

### 7.4 Framing, and why `recv()` returns partial data

`recv()` returns *whatever bytes have arrived so far*, which may be:

- **less than one command** — the client's `send()` was split across packets, or the data crossed an MTU boundary, or the sender's TCP buffer flushed early;
- **exactly one command** — the common case, and the one that lets buggy servers pass casual testing;
- **several commands at once** — the client pipelined, or Nagle coalesced them;
- **two and a half commands** — the general case, and the one you must actually write code for.

None of these is an error. TCP promises a byte *stream*; it never promised to
preserve the boundaries between writes. A server that assumes "one `recv()` =
one request" works perfectly on loopback with a slow hand-typed client and
corrupts itself the moment a real client pipelines.

`LineBuffer` is the answer, and it is deliberately socket-free so that every
awkward split can be reproduced in a unit test by choosing where to cut the
input:

```cpp
while (true) {
  // Drain every complete line already buffered BEFORE asking for more bytes.
  // A client that pipelines would hang forever if we served one request per read.
  while (const auto line = buffer_.next_line()) {
    if (!handle_line(*line)) return;
  }
  if (buffer_.buffered() > kMaxLineBytes) { /* refuse and close */ }
  if (!fill_buffer()) return;   // recv() == 0 means the peer is gone
}
```

The mirror problem exists on the way out. **`send()` may accept fewer bytes than
offered** — the kernel's send buffer can simply be full — so `send_all()` loops
until the whole reply is gone. Ignoring `send()`'s return value is one of the
classic socket bugs: the reply goes out truncated and the peer's stream is
corrupted from then on.

### 7.5 Parser design

```cpp
struct ParseResult { bool ok; Command command; std::string error; };
ParseResult parse_command(std::string_view line);
```

- **`string_view` in, no allocation while scanning.** Tokens are views into the caller's line; only the accepted `Command` copies anything.
- **A result type, not exceptions.** A malformed command is an ordinary, expected event on a public socket — clients send garbage constantly. That is not an exceptional condition, and making it one would put a throw/catch on the hot path of a server's most common failure mode.
- **`std::from_chars`, not `atoi`/`stoll`.** No exceptions, no locale, and — critically — it reports where parsing stopped, so `12abc` is *rejected* rather than silently read as `12`. `atoi` would accept it.
- **The parser never sees a `\n`.** Framing already removed it. One job each.
- **Verbs are case-insensitive; keys and values are not.** Typing `get foo` by hand should work; `Key` and `key` are different keys.

### 7.6 Error handling

The rule: **a bad command is a reply, not a disconnection.**

| Situation | Response | Connection |
| --- | --- | --- |
| Unknown verb, wrong arity, bad TTL, empty line | `-ERR <reason>` | **stays open** |
| Request line over 64 KiB | `-ERR line too long...` | **closed** |
| Client disconnects (`recv` → 0) | — | closed |
| `send()` fails mid-reply | — | abandoned |

The single exception — the over-long line — is a *framing* failure rather than a
semantic one: the server no longer knows where the next command begins, so there
is nothing safe to do but hang up. Redis behaves the same way. Recovering by
discarding input until the next newline would be possible; closing is simpler and
this is a protocol violation, not a typo.

Two hazards the server has to survive, both tested:

- **Writing to a peer that has vanished** raises `SIGPIPE`, whose default action is to *kill the process*. A server that dies because a client hung up is unacceptable. Suppressed with `MSG_NOSIGNAL` (Linux) or `SO_NOSIGPIPE` (macOS/BSD).
- **`EINTR`** — a signal arriving mid-syscall — means "nothing happened, try again", not "failure". Treating it as an error is a classic source of rare, unexplainable dropped connections.

### 7.7 Input limits

| Limit | Value | Why it exists |
| --- | --- | --- |
| Request line | 64 KiB | **Without it, a client that never sends `\n` makes the server buffer until it runs out of memory.** One missing check between a working server and a trivial denial of service. |
| TTL | ≤ 10 years | Guards the deadline arithmetic against overflow. |
| Echoed tokens in errors | 32 chars | So a 60 KB junk token cannot be reflected back in full. |
| Accept backlog | 128 | Bounds the kernel's queue of waiting connections. |

The general principle: **every buffer that grows in response to input needs a
bound**, and the bound belongs at the layer that knows the policy. `LineBuffer`
deliberately does *not* enforce the line limit — it reports `buffered()` and lets
`Connection` decide, which keeps policy out of the framing code.

### 7.8 Connection lifecycle and concurrency

One connection carries any number of commands; opening a connection per request
would pay a full three-way handshake each time. It ends in one of three ways:
`QUIT` (acknowledged with `+BYE` *before* the close, so the client never has to
guess), client disconnect (`recv` → 0), or a framing violation.

*Historical note — superseded in Stage 6.* **The Stage 5 server served exactly
one client at a time.** `accept()` returned a connection, it was served to
completion, and only then was the next one accepted; a second client's
`connect()` succeeded — the kernel completes the handshake — and then waited in
the backlog. That was deliberate while the cache was not thread-safe, and the
single-client network benchmark was recorded then as the baseline for what came
next.

**Today** each accepted connection gets its own worker thread and the cache is a
`ShardedCache`; see §8 and §9. `accept()` still blocks indefinitely, so the
accept loop still waits on `poll()` with a 100 ms timeout and re-checks the
`std::atomic<bool>` set by `Server::stop()` — that is what lets another thread
shut the server down, after which every worker is joined.
---

## 8. Concurrency

### 8.1 The model: thread-per-connection + one mutex

```
   client 1 ──┐                          ┌─ worker thread 1 ─┐
   client 2 ──┤    accept loop           ├─ worker thread 2 ─┤
   client 3 ──┤  (its own thread,        ├─ worker thread 3 ─┤
     ...      │   only accepts)          │       ...         │
   client N ──┘         │                └─ worker thread N ─┘
                        │                          │
                   spawns one ───────────────────► │  all N threads
                   thread per                      │  share ONE cache
                   connection                      ▼
                                     ┌──────────────────────────┐
                                     │  SyncCache               │
                                     │  ┌────────────────────┐  │
                                     │  │  std::mutex        │  │ ◄── the only
                                     │  └────────────────────┘  │     shared
                                     │  Cache                   │     state
                                     │   ├─ unordered_map       │
                                     │   └─ RecencyList         │
                                     └──────────────────────────┘
```

Each connection gets its own thread. Everything a worker touches is private to it
— its `Socket`, its `LineBuffer`, its parsed `Command` — **except the cache**,
which is shared by all of them and guarded by a single mutex.

Thread-per-connection is the simplest model that actually works, which is why it
is the baseline. It does not scale to many thousands of connections: each thread
costs a stack and a scheduler slot, so `Options::max_connections` (default 256)
caps it and a client arriving past the limit is refused politely rather than
being allowed to exhaust memory.

### 8.2 Mutex ownership and critical sections

The mutex lives **inside `SyncCache`**, not in the server, and never leaves it.
Callers cannot lock, unlock, or forget to. Each public method is one critical
section: take the lock, delegate to the single-threaded `Cache`, release.

```cpp
std::optional<std::string> get(const std::string& key) {
  const std::lock_guard<std::mutex> lock(mutex_);   // critical section starts
  return cache_.get(key);
}                                                   // ...and ends here, always
```

`lock_guard` means the lock is released on every path out, including an
exception. The critical section is exactly one cache operation — small, fixed,
and containing no I/O. **No socket call ever happens while the lock is held**,
which is what keeps a slow or dead client from blocking every other client.

`Cache` itself is untouched and still single-threaded. Keeping the mutex in a
wrapper means the engine pays nothing in the in-process benchmarks, and the
locking is visible in one file instead of scattered through the data structure.

A second, much smaller mutex (`Server::workers_mutex_`) guards only the vector of
worker threads. It is never held while serving a connection, and never held at
the same time as the cache mutex — two locks that can never be taken together
cannot deadlock.

### 8.3 Why GET needs an exclusive lock

This is the part that catches people out.

**Under LRU, a read is a write.** `get()` does not merely look a key up:

1. it splices the entry to the head of the recency list — three pointer updates to shared memory;
2. if the entry has expired, it *deletes* it from both the map and the list.

So two threads "just reading" are two threads mutating the same linked list. Left
unguarded, that is a classic corrupted list: nodes lost, cycles created, or a
node freed while another thread walks through it.

This is not theoretical. Running the unsynchronised `Cache` from four threads
under ThreadSanitizer produced **92 data-race reports** and then hung — a
corrupted list had become a cycle. §8.4 has the detail.

The consequence for design: **`std::shared_mutex` would buy almost nothing
here.** Readers can only share a lock if they are genuinely read-only, and in a
cache the common operation is `get` — which is a writer. Only `contains` and
`size` could take a shared lock, and they are the rare ones. That is worth
knowing before reaching for a reader/writer lock as an "obvious" improvement.

### 8.4 Race conditions, and what the lock does not fix

`SyncCache` makes every **individual operation** atomic. It does not make
**sequences** of operations atomic, and the difference matters:

```cpp
int n = std::stoi(cache.get("counter").value_or("0"));   // ← another thread
cache.set("counter", std::to_string(n + 1));             //   can run here
```

Two threads can read the same value and both write back the same increment, so
one update is lost. This is a lost-update race and it survives a perfectly
thread-safe cache. `tests/concurrency_test.cpp` asserts the bug rather than
hiding it, because it is an honest statement of the contract. Fixing it needs a
compound operation *inside* the lock (an `INCR` command) or a compare-and-swap —
not a bigger mutex.

**Verification.** The suite runs under ThreadSanitizer with zero warnings. That
result is only meaningful if TSan would catch a race in this code, so that was
checked directly: a four-thread probe against the raw, unsynchronised `Cache`
reported 92 races — pointing at `cache.cpp:58`, the `index_.emplace` that
triggers a rehash — and then hung. The tooling works; the clean run is real.

### 8.5 Thread lifecycle

```
  accept() returns a socket
        │
        ├─► spawn_worker(): create thread, push {thread, finished flag} onto workers_
        │        │
        │        └─► worker: Connection(socket, cache).serve()
        │                      ... reads commands until the client goes away ...
        │                    set finished = true       ← last thing it does
        │
        ├─► each accept-loop iteration: reap_finished_workers()
        │        join() and erase every worker whose flag is set
        │
        └─► stop() → loop exits → join_all_workers()
```

Workers are **joined, never detached**. A detached worker holds a reference to
the cache and could outlive it, which is a use-after-free waiting for a shutdown
to happen at the wrong moment. Joining means `run()` cannot return until every
worker is finished.

`std::thread` cannot be asked "are you done?", so each worker sets a shared
`atomic<bool>` on its way out and the accept loop reaps the finished ones. The
flag is set **last**, after everything else the thread does, because once it is
true the accept loop may join and destroy that `Worker` entry.

`join_all_workers()` swaps the vector out under the lock and joins *outside* it —
joining while holding `workers_mutex_` could block on a worker that is itself
trying to take that mutex.

### 8.6 Bottlenecks — measured, not assumed

The obvious story is "one global mutex serialises everything, so that is the
bottleneck". **The measurements say otherwise**, and the benchmark includes two
controls specifically to test it.

| clients | 1 | 2 | 4 | 8 | 16 |
| --- | --- | --- | --- | --- | --- |
| **PING** — takes no lock at all | 1.00x | 1.66x | 1.76x | 2.41x | 2.46x |
| **GET** — takes the lock | 1.00x | 1.77x | 1.88x | 2.53x | 2.54x |
| **SET** — takes the lock | 1.00x | 1.82x | 1.97x | 2.56x | 2.66x |

`PING` touches no cache data and takes no lock, yet it plateaus in the same
place. **So over TCP the mutex is not what limits scaling** — the transport,
syscalls and scheduler are. That follows from Stage 5's finding that a round trip
is ~20 µs while the cache operation is ~0.4 µs: a 2% serial section cannot cap
speedup at 2.5x.

Remove the network, though, and the mutex has nowhere to hide:

| threads calling `SyncCache::get()` directly | 1 | 2 | 4 | 8 | 16 |
| --- | --- | --- | --- | --- | --- |
| scaling | 1.00x | 0.83x | 0.48x | 0.61x | 0.70x |

**Adding threads makes it slower.** Throughput never exceeds the single-threaded
figure, because the lock admits exactly one thread at a time and the extra
threads only add contention and handoff cost. That is what a hard sequential
section looks like, and it is the real argument for sharding: not that the lock
is slow today, but that it is a wall the system will hit the moment the transport
stops being the limit.

The third signature to recognise: across all three TCP tables, **p50 latency
rises roughly in proportion to client count (20 µs → 128 µs at 16) while
throughput stays flat**. Growing queue, constant service rate — the definition of
a saturated resource.
---

## 9. Sharding

### 9.1 Design

```
                          ShardedCache
                               │
        ┌──────────────┬───────┴───────┬──────────────┐
        │              │               │              │
     Shard 0        Shard 1         Shard 2       Shard N-1
   ┌──────────┐   ┌──────────┐   ┌──────────┐   ┌──────────┐
   │ mutex    │   │ mutex    │   │ mutex    │   │ mutex    │
   │ hash map │   │ hash map │   │ hash map │   │ hash map │
   │ LRU list │   │ LRU list │   │ LRU list │   │ LRU list │
   │ capacity │   │ capacity │   │ capacity │   │ capacity │
   └──────────┘   └──────────┘   └──────────┘   └──────────┘
```

Each shard is a complete, independent `SyncCache`. Nothing is shared between
shards — no cross-shard lock, and therefore no lock ordering to get wrong and no
way to deadlock.

Every operation locks **exactly one** shard, chosen by hashing the key. Threads
working on keys in different shards never wait for each other. One global mutex
makes the cache one sequential section; N shards make it N sequential sections
that run in parallel.

`ShardedCache` exposes the identical API to `SyncCache`, so **the network layer
does not know sharding exists** — changing the shard count does not touch a line
of it. Shard count is a constructor argument and a server command-line flag.

### 9.2 Shard selection

```cpp
std::uint64_t h = std::hash<std::string>{}(key);
h ^= h >> 30;  h *= 0xbf58476d1ce4e5b9ULL;   // splitmix64 finalizer
h ^= h >> 27;  h *= 0x94d049bb133111ebULL;
h ^= h >> 31;
shard_index = h % shard_count;
```

**Why the extra mixing step, rather than `std::hash(key) % shard_count`?**
Because the shard's own `unordered_map` also hashes the key with `std::hash` to
pick a bucket. Using the raw value for both means every key in a shard shares
`h % shard_count`, which correlates with the bucket index whenever the shard
count and bucket count share a factor — clustering keys into a few buckets of
each shard's map and quietly degrading lookups. The finalizer is three shifts and
two multiplies, and it decorrelates the two uses.

A test checks the distribution directly: 8000 keys across 8 shards must land
within ±25% of even. Clustering would defeat the whole point — crowded shards
would contend exactly as badly as one global mutex.

### 9.3 Capacity distribution

Total capacity is split as evenly as possible, with the remainder handed to the
first few shards so the parts sum to **exactly** the requested total (10 across
4 shards → 3, 3, 2, 2 — not 2, 2, 2, 2).

If the total capacity is smaller than the shard count, the shard count is
**reduced** so no shard gets capacity 0. A zero-capacity shard would accept no
keys at all, and every key hashing to it would vanish silently — a memorable bug,
and easy to prevent here.

### 9.4 Correctness trade-offs

Two things genuinely change, and both are the price of removing the shared lock.

**1. LRU is per-shard, not global.** Each shard evicts its own least recently
used entry knowing nothing about the others. A hot key in a crowded shard can be
evicted while a colder key in a quiet shard survives. True global LRU would need
a single recency list — which is the very thing being removed.

*Measured cost:* on a skewed 80/20 workload, **≤0.02 percentage points** of hit
rate across 1/2/4/8 shards at every capacity tested (§9.6). Effectively free
here, because the hash spreads keys evenly enough that each shard sees a
statistically similar slice of the distribution. It would not be free with very
few shards, a small capacity, and a hot set that happened to concentrate in one
shard — the general claim is *"cheap when keys distribute evenly"*, not *"free"*.

**2. `size()` is not a consistent snapshot.** It locks each shard in turn and
sums, so another thread can modify a shard that has already been counted. It is
exact on a quiescent cache and approximate under load. A globally consistent
count would need every shard locked at once — reintroducing precisely the
bottleneck sharding exists to remove. The same applies to `evictions()` and
`expired_removals()`.

Everything a single-key operation can observe is unchanged: `get`, `set`,
`erase`, `contains` and `ttl` behave identically at any shard count, and a test
asserts exactly that against 1, 4 and 16 shards.

### 9.5 Lock contention, and why sharding addresses it

With one mutex, every operation queues behind every other one regardless of which
key it touches. Threads do not get faster by waiting; they take turns. Stage 6
measured this directly: N threads on one mutex scaled 1.00x → 0.83x → 0.48x —
**adding threads made it slower**, because the extra threads contributed only
contention and handoff cost.

Sharding replaces one queue with N queues. Two threads collide only when their
keys hash to the same shard, which for N shards and well-distributed keys happens
about 1/N of the time.

The limit is worth stating: sharding reduces contention, it does not eliminate
it. Keys are not guaranteed to spread evenly, and a genuinely hot single key puts
all its traffic on one shard no matter how many shards exist.

---

## Measured Performance Improvements

Every number below is benchmark output from `cachex_net_bench` on this machine
(Apple M2 Pro, 12 hardware threads, AppleClang 21, `-O3 -DNDEBUG`). Identical
workload and machine conditions across all configurations; 32,000 requests per
networked configuration, 600,000 per in-process configuration.

**Version A = 1 shard (one global mutex), B = 2 shards, C = 4, D = 8.**

### In-process — `ShardedCache::get()` called directly, no sockets

Throughput in ops/sec:

| threads | A (1 shard) | B (2) | C (4) | D (8) | best vs A |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 7,146,193 | 6,885,913 | 6,941,097 | 6,855,821 | **+0.0%** |
| 2 | 3,853,705 | 3,837,794 | 4,093,006 | 4,882,059 | **+26.7%** |
| 4 | 2,252,335 | 3,376,833 | 4,501,711 | 7,477,789 | **+232.0%** |
| 8 | 3,289,281 | 2,466,958 | 3,060,406 | 4,685,978 | **+42.5%** |
| 16 | 3,205,524 | 2,243,006 | 2,855,006 | 4,103,501 | **+28.0%** |

The 4-thread result reproduced across three runs at **+227%, +232%, +240%**
(2.2M → 7.3–7.5M ops/sec, i.e. **3.3x**). The 1-thread result reproduced at
**+0.0%** every time.

Two things this says plainly:

- **With no contention there is nothing to win.** At 1 thread, sharding is flat to very slightly negative — it adds a hash and an indirection and removes no waiting, because there was none.
- **Under contention the win is large and scales with shard count.** At 4 threads the ordering is monotonic: 2.25M → 3.38M → 4.50M → 7.48M for 1 → 2 → 4 → 8 shards.

An oddity worth flagging rather than explaining away: the 1-shard column *rises*
from 4 to 8 threads (2.25M → 3.29M). More contention should not be faster. The
likely cause is lock-handoff batching — under heavy contention a thread that
releases and immediately reacquires keeps the cache line locally, reducing
cross-core traffic — but that was not verified, so it is a hypothesis, not a
finding.

### Over TCP — the full A/B/C/D × 1/2/4/8/16 matrix

GET, throughput in req/sec:

| clients | A (1 shard) | B (2) | C (4) | D (8) | best vs A |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 48,510 | 48,499 | 48,469 | 48,005 | +0.0% |
| 2 | 85,206 | 84,637 | 83,432 | 84,186 | +0.0% |
| 4 | 89,629 | 89,849 | 88,865 | 90,380 | +0.8% |
| 8 | 122,006 | 121,745 | 121,547 | 122,563 | +0.5% |
| 16 | 124,484 | 124,782 | 123,071 | 123,028 | +0.2% |

GET, p99 latency in µs:

| clients | A (1 shard) | B (2) | C (4) | D (8) | best vs A |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 28.21 | 28.71 | 28.12 | 27.62 | −2.1% |
| 2 | 36.38 | 36.38 | 40.79 | 37.29 | +0.0% |
| 4 | 56.38 | 57.38 | 56.04 | 56.50 | −0.6% |
| 8 | 84.12 | 84.83 | 83.38 | 82.71 | −1.7% |
| 16 | 148.42 | 149.62 | 170.12 | 176.75 | +0.0% |

Scaling vs 1 client is ~2.6x at 16 clients for **every** shard count — 1 shard
included.

**Over TCP, sharding changes nothing measurable.** Throughput moves by ≤0.8% and
p99 by ≤2%, both inside run-to-run noise, and SET behaves the same way.

That is not a contradiction of the in-process result; it is the same finding seen
through a different bottleneck. Stage 5 measured a round trip at ~20 µs against a
~0.4 µs cache operation, and Stage 6's `PING` control — which takes no lock at
all — plateaued exactly where `GET` did. The transport, syscalls and scheduler
cap throughput long before the cache mutex does, so removing contention from a
2% slice of the request is invisible end to end.

### Global LRU vs per-shard LRU — the correctness cost

Hit rate on a skewed 80/20 workload, cache-aside, single-threaded:

| capacity | A (1 shard) | B (2) | C (4) | D (8) | cost |
| --- | ---: | ---: | ---: | ---: | ---: |
| 5% (5,000) | 15.92% | 15.91% | 15.93% | 15.90% | −0.01pp |
| 10% (10,000) | 30.92% | 30.92% | 30.91% | 30.93% | −0.02pp |
| 20% (20,000) | 57.78% | 57.79% | 57.81% | 57.78% | +0.00pp |
| 40% (40,000) | 84.21% | 84.21% | 84.21% | 84.20% | −0.01pp |

Giving up global LRU cost at most **0.02 percentage points** of hit rate.

### Conclusions

**When sharding helps.** When threads contend for the cache itself and the keys
distribute evenly — the in-process case, where it delivered **3.3x at 4 threads**.
That is the honest headline number, and it is an in-process figure, not an
end-to-end one.

**When sharding does not help.**

- **When there is no contention.** At 1 thread it is +0.0%: a hash and an indirection bought in exchange for waiting that never happened.
- **When something else is the bottleneck.** Over TCP the transport dominates, and sharding is worth ≤0.8%. This is the important one: optimising a component that is not the limiting factor produces no end-to-end change, however good the microbenchmark looks.
- **When the load is skewed onto one key.** All its traffic lands on one shard regardless of shard count.
- **When a globally consistent view is required.** `size()` across shards is approximate under load.

**What this says to do next.** The measurements point at the I/O model, not the
cache: an event loop instead of a thread per connection attacks the bottleneck
that is actually binding. Sharding is now in place for when that ceiling lifts —
which is the right order, because it was chosen from data rather than from
intuition about which part *looked* slow.
---

## 10. Persistence

### 10.1 Architecture

```
   ShardedCache  ──export_entries()──►  PersistenceManager  ──►  snapshot file
        ▲                                      │
        └────────────  set() / set(ttl)  ──────┘
```

`PersistenceManager` talks to the cache **only through its public API**:
`export_entries()` to read, ordinary `set()` calls to restore. It never touches a
map, a list or an iterator, and it knows nothing about sockets — the `SAVE` and
`LOAD` commands are wired up in the network layer, which passes it a pointer.

`export_entries()` is the one new abstraction the cache had to grow, and it
deliberately returns a plain value type:

```cpp
struct EntrySnapshot {
  std::string key;
  std::string value;
  std::optional<std::chrono::milliseconds> remaining_ttl;  // nullopt = forever
};
```

No iterators, no deadlines tied to this process. That shape is what keeps
persistence from becoming a second implementation of the cache's internals.

Persistence is **opt-in**: with no snapshot path configured, `SAVE` and `LOAD`
reply `-ERR persistence is not enabled on this server` rather than writing
somewhere unexpected.

### 10.2 Format

```
CACHEX-SNAPSHOT 1 <saved_at_unix_ms>\n
<entry_count>\n
  <key_len> <value_len> <ttl_ms>\n        ← repeated entry_count times
  <key bytes><value bytes>\n
```

`ttl_ms` is `-1` for an entry that never expires, otherwise the milliseconds
remaining **at the moment of the save**.

**Length-prefixed, not delimited.** The lengths come first and the payload is
copied verbatim, so a key or value may contain spaces, newlines or NUL bytes —
everything the cache can actually hold. This is exactly the framing
`docs/PROTOCOL.md` names as the fix for the wire protocol's whitespace
limitation; the snapshot gets it because nothing here has to be typed by a human.

The trailing `\n` after the payload is redundant given the lengths, which is
precisely why it is checked: if it is missing, the lengths disagreed with the
file and everything after that point is garbage.

**Why the header carries a wall-clock timestamp.** TTLs are tracked on
`steady_clock`, which is monotonic but whose epoch is meaningless outside the
process, so the file cannot store deadlines — only durations. A duration alone is
not enough either: a key saved with 60 ms left would come back with a *fresh*
60 ms however long the server was down. The loader needs to know how much
wall-clock time passed, so `system_clock` is used for that one job and nothing
else. This was found by a test, not by inspection — the first version had the bug.

Elapsed time is clamped at zero. If the wall clock moved backwards between save
and load, the subtraction would *extend* every TTL; refusing to go negative means
the worst a clock jump can do is keep a key slightly too long, never resurrect
one that should be gone.

### 10.3 Snapshot lifecycle

```
   server start ──► load(): restore entries, skipping any whose TTL ran out
                     │        (a missing file is normal, not an error;
                     │         a corrupt file is logged and the server
                     │         starts empty rather than refusing to run)
                     ▼
   running ──────► SAVE command   → synchronous write, replies with the count
                └► PeriodicSaver  → optional background thread, on an interval
                     ▼
   clean shutdown ─► save(): one final snapshot
```

A `SAVE` is synchronous: the connection that asked waits for the write. Redis's
`SAVE` behaves the same way, and its `BGSAVE` — fork the process and let the
child write a copy-on-write snapshot — is far more machinery than this stage
wants.

`PeriodicSaver` waits on a condition variable rather than sleeping, so stopping
is immediate instead of taking up to a full interval, and it is **joined in its
destructor, never detached** — a detached saver could outlive the cache it
references.

### 10.4 Consistency under concurrent access

Two separate questions, and it is worth keeping them apart.

**Is the output well-formed while clients are writing? Yes.** Each shard is
locked, copied, and unlocked, so every record written is a whole record. There is
no path that produces a torn key or a half-written value. A test runs eight saves
while six threads churn the cache and checks every snapshot loads cleanly.

**Is it a single point in time? No.** Shards are locked one at a time, so shard 0
is read slightly before shard N-1 — a key written between the two appears or not
depending on which shard it lives in. This is the same trade `size()` already
makes (§9.4), and for the same reason: locking every shard at once would give a
true instant and would reintroduce exactly the global stall sharding exists to
remove.

**The file is never open while a lock is held.** `save()` copies the cache out
first, then writes. A slow disk cannot stall the request path — it only delays
the connection that issued `SAVE`.

**Writes are atomic.** The snapshot goes to `path.tmp` and is then `rename()`d
into place. POSIX `rename` is atomic, so an interrupted save leaves either the
previous snapshot or the new one, never a truncated file. A test verifies that a
failed save leaves the previous snapshot byte-for-byte intact.

**Loads are all-or-nothing.** The file is parsed and validated completely before
a single entry is applied, so a corrupt snapshot leaves the cache exactly as it
was rather than half populated.

### 10.5 Failure modes

| Failure | Behaviour |
| --- | --- |
| No snapshot file | `ok`, zero entries — the normal state on a first start |
| Zero-byte file | Rejected. A truncated file is not an empty cache, and treating it as one would hide a real failure |
| Wrong magic / unsupported version | Rejected with a reason; cache untouched |
| Truncated or corrupt record | Rejected with the record index; cache untouched |
| Length disagrees with payload | Caught by the trailing-newline check |
| TTL ran out while on disk | Entry skipped, counted in `LoadResult::expired` |
| Disk full / unwritable path | `save()` fails, temp file removed, previous snapshot intact |
| Crash mid-save | Previous snapshot intact (the rename never happened) |
| Crash between saves | **Everything since the last save is lost** — see §10.7 |
| Corrupt snapshot at startup | Logged; the server starts with an empty cache rather than refusing to run. A cache that will not start is worse than a cold one |

### 10.6 Benchmark results

`cachex_persist_bench`, Release, 16-byte keys and 64-byte values, 8 shards,
median of 5, on this machine's `/tmp`:

| entries | save | load | save µs/entry | snapshot | bytes/entry |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1,000 | 0.75 ms | 0.52 ms | 0.751 | 87.9 KiB | 90.0 |
| 10,000 | 4.55 ms | 4.67 ms | 0.455 | 878.9 KiB | 90.0 |
| 100,000 | 50.54 ms | 48.51 ms | 0.505 | 8.6 MiB | 90.0 |
| 500,000 | 264.00 ms | 252.25 ms | 0.528 | 42.9 MiB | 90.0 |

With a TTL on every entry: 95.0 bytes/entry, and 302 ms / 294 ms at 500,000 —
the extra five bytes are the millisecond count.

**Both scale linearly** at roughly **0.5 µs per entry** each way, which is what a
single pass over the data with no index to rebuild should look like.

**Snapshot size against memory:**

| | bytes/entry | vs payload |
| --- | ---: | ---: |
| Payload (key + value) | 80 | 1.00x |
| Snapshot on disk | 90 | 1.13x |
| Live cache in memory | **221 (measured)** | 2.76x |

The snapshot is **roughly half the size of the live cache**, because it stores
only the data. The hash map's buckets and nodes, both list pointers, the
duplicated key (§10, the eviction trick) and the allocator's overhead all exist
to make lookups O(1) — none of it is worth writing down, because loading rebuilds
it. The in-memory figure was an estimate when this was written and was measured
in Stage 9 at **221 bytes/entry**, so the snapshot is ~2.5× smaller than the live
cache.

The number with an operational consequence: **a synchronous `SAVE` of 500,000
entries blocks the connection that issued it for ~264 ms.** Other clients are
unaffected — the save holds no lock while writing — but that client is stalled.

### 10.7 What this is NOT

CacheX persistence is a **snapshot**, and that word is doing a lot of work. It is
not a database, and the gaps are not subtle:

| | CacheX | A production database |
| --- | --- | --- |
| **Durability** | Everything written since the last `SAVE` is lost on a crash | A write-ahead log makes every committed write durable before it is acknowledged |
| **`fsync`** | **Not called.** The data is handed to the OS, not forced to the platter. A power loss can lose a snapshot that `save()` reported as successful | `fsync` on the log (and the directory) before acknowledging |
| **Atomicity** | Per-snapshot only | Per-transaction, with rollback |
| **Point-in-time consistency** | Per shard, not global (§10.4) | A true consistent snapshot (MVCC or a fork) |
| **Incremental** | Full rewrite every time — O(n) per save regardless of how little changed | Append-only log; cost is proportional to the change |
| **Recovery** | Load the whole file, or start empty | Replay the log from the last checkpoint |
| **Replication** | None | Snapshots ship to replicas; the primary drives expiry |
| **Compaction / compression** | None. Plain text, no encoding | Compressed pages, background compaction |
| **Format stability** | Version 1, no upgrade path written yet | Documented, versioned, migratable |
| **Integrity** | Length checks only — no checksum, so silent bit-rot is not detected | Per-page checksums |

The honest framing: this makes a **cache** survive a *planned* restart. It does
not make CacheX a system of record, and data in it should always be
reconstructible from somewhere else. That is true of any cache — it is just
easier to forget once there is a file on disk.

⬜ The three things that would most change that picture, roughly in order of
value per unit of work: `fsync` before the rename (real crash durability), a
checksum per record (integrity), and an append-only log of changes between
snapshots (bounded data loss).






---

## 11. Complexity

All bounds assume a hash function that distributes keys reasonably.

| Operation | Average | Worst case | Where the worst case comes from |
| --- | --- | --- | --- |
| `set` (insert, below capacity) | **O(1)** | O(n) | All keys collide into one bucket; or the insert triggers a rehash |
| `set` (insert, **at capacity**) | **O(1)** | O(n) | Same. The eviction itself is O(1) — see §5.3 |
| `set` (update) | **O(1)** | O(n) | Bucket collisions only — no rehash, no allocation, no eviction |
| `get` | **O(1)** | O(n) | Bucket collisions. The splice is always O(1) |
| `erase` | **O(1)** | O(n) | Bucket collisions |
| `contains` | **O(1)** | O(n) | Bucket collisions |
| `ttl` | **O(1)** | O(n) | Bucket collisions |
| **Lazy expiry check** | **O(1)** | O(1) | One `optional` test, then at most one clock read and a compare |
| **Reclaiming an expired entry** | **O(1)** | O(1) | The entry is already in hand; same teardown as any removal |
| **LRU eviction** | **O(1)** | **O(1) amortised** | Finding the victim is O(1) (the tail); removing its index entry is an average-O(1) hash erase |
| `size` / `empty` / `capacity` / `evictions` | **O(1)** | O(1) | Counters and `unordered_map::size` |
| `clear` | **O(n)** | O(n) | Every node must be destroyed |
| `keys_by_recency` | **O(n)** | O(n) | Diagnostic only; walks the list and copies every key |

**Nothing scans the cache**, for eviction or for expiry. The eviction candidate is
always `entries_.oldest()` — the tail of the list — reachable in constant time,
and expiry is only ever checked on an entry an operation is already holding.

**Space: O(n)**, bounded by `capacity` once one is set. Per entry, roughly:
the key twice (§12), the value once, two list pointers, and the map's node and
bucket overhead. For a 16-byte key and a 64-byte value this is **221 bytes
measured** against ~80 bytes of payload — 2.76×. See Performance Evaluation.

*(This was carried as an estimate of "~180–200 bytes" from Stage 3 until Stage 9
measured it. The layout calculation was too low, mostly because it did not
account for the allocator's per-allocation bookkeeping across two nodes per
entry.)*

A caveat that matters more than the table: **every operation here is O(1), and
they still differ by roughly 10× in measured cost** (§13). The constant factors —
allocation, copying, and memory locality — dominate at this scale. The table is
the right answer to "how does this scale?" and the wrong answer to "which is
fastest?".

---

## 12. Ownership and Lifetime

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

Both structures must therefore be updated together, and there are two places
where the order is not interchangeable:

```cpp
// erase(key)
entries_.erase(it->second);   // read it->second while the map entry still exists
index_.erase(it);             // only now drop the handle

// evict_oldest()
const std::string& victim_key = entries_.oldest().key;
index_.erase(victim_key);     // erasing from the map does not touch the list...
entries_.pop_oldest();        // ...so victim_key stays valid until here
```

Get either order wrong and it is a genuine use-after-free — and one that will
usually *appear* to work, because the freed memory is typically untouched. This
is exactly what `-fsanitize=address` catches, which is why the suite is run under
ASan/UBSan on every change.

### Why the key is stored twice

`Entry::key` duplicates the map's key. That is a real cost — roughly 16–32 extra
bytes per entry — and it is deliberate: eviction runs list → map and needs the
victim's key to find its index entry (§5.3).

⬜ *Future (Stage 11):* the node could instead hold `const std::string*` pointing
at the map's key, which is stable across rehashing because `unordered_map`
rehashing invalidates *iterators* but not *pointers or references* to elements.
That removes the duplication, but it makes `evict_oldest` subtle in a new way —
`index_.erase(*victim.key_ptr)` would be passing the map a reference to its own
key while asking it to destroy the node that owns it. It is a measured
optimisation for later, not a default.

### Why `get` returns a copy

Returning `const std::string&` or a pointer would be faster and would be a trap.
The reference stays valid only until the next call that touches that entry — and
**now that eviction exists, any `set` on a full cache can be that call**, evicting
an entry the caller has nothing to do with. A caller holding a reference across
two calls would read freed memory, and it would usually appear to work.

The safe default is a copy. Once there is a network server the value is
serialised to a socket buffer immediately anyway, so the copy largely disappears
into work that has to happen regardless.

⬜ *Future:* `std::shared_ptr<const std::string>` values would make sharing safe
and cheap without copying the bytes, at the cost of an atomic refcount — a very
different trade-off now that Stage 6 made the cache multi-threaded.

✅ *Partly addressed in Stage 9:* profiling showed the per-hit allocation behind
this copy was a real share of the read path, so `get_into(key, buffer)` was added.
It still copies — so the safety argument above is untouched — but into a
caller-owned buffer whose capacity is reused, which measured +45% throughput
(median of 5 runs) on the read-heavy workload.

### No hidden insertion

`get` and `contains` use `find`, never `operator[]`. `operator[]` on an
`unordered_map` **default-constructs a value when the key is absent**, so a
lookup miss would silently insert an entry — and in a bounded cache that means a
stream of misses would evict real data to make room for empty placeholders. Two
tests pin this (`get_missing_key_returns_nullopt`,
`contains_does_not_insert_on_a_miss`).

---

## 13. Benchmark Methodology

Four benchmark binaries, one capture script:

| Binary | Source | What it measures | Since |
| --- | --- | --- | --- |
| `cachex_bench` | `benchmarks/cache_benchmark.cpp` | In-process core operations, LRU cost, workloads, hit-rate curve, TTL cost | Stages 2–4 |
| `cachex_net_bench` | `benchmarks/net_benchmark.cpp` | Over TCP: PING/GET/SET, 1–16 clients, shard matrix | Stages 5–7 |
| `cachex_persist_bench` | `benchmarks/persistence_benchmark.cpp` | Snapshot save/load time and size | Stage 8 |
| `cachex_bench_suite` | `benchmarks/bench_suite.cpp` | 5 workloads × 3 versions × 5 thread counts, memory, `get_into` before/after | Stage 9 |

Shared timing and percentile code lives in `benchmarks/bench_util.hpp`.

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DCACHEX_BUILD_BENCHMARKS=ON
cmake --build build/release -j
./benchmarks/run_all.sh          # runs all four, 5 suite runs by default via RUNS=5
```

`run_all.sh` writes raw output to `benchmarks/results/` together with
`environment.txt` (machine, OS, compiler, git commit, dirty flag, load average).
Those files are committed, so every number quoted in this document or in
`benchmarks/RESULTS.md` can be found in one of them. The per-stage write-ups are
in `benchmarks/RESULTS.md`; the current, authoritative numbers are in
**Performance Evaluation** at the end of this document.

The rest of this section describes the in-process harness from Stages 2–4; the
suite's methodology is under Performance Evaluation → Methodology.

### What is measured

| § | Section | Purpose |
| --- | --- | --- |
| 1 | Core operations | Six phases (`SET insert/update`, `GET hit/miss`, `CONTAINS`, `ERASE`) on an unbounded cache — continuity with the Stage 2 baseline |
| 2 | Cost of LRU | The same request sequence against an unbounded cache, a bounded cache with headroom, and a bounded cache that evicts |
| 3 | Workload A | 90% GET / 10% SET, skewed keys, hit-dominated |
| 4 | Workload B | 20% GET / 80% SET, uniform keys, eviction-dominated |
| 5 | Hit rate vs capacity | The point of LRU, as a curve |
| 6 | Cost of TTL checks | GET hits with no TTL, with a TTL, and over expired entries |

A second binary, `cachex_net_bench`, measures the same cache **over TCP**:

| Phase | Purpose |
| --- | --- |
| `PING` | The transport floor — a full round trip that touches no cache data |
| `SET` | Round trip including a cache write |
| `GET` | Round trip including a cache read |

Its methodology, and how it differs:

| Decision | Reason |
| --- | --- |
| Server runs in-process, on a thread, over loopback | One self-contained binary, and no doubt about which build of the server is being measured. The cost is that no real network is involved — loopback skips the NIC, the wire, and most of the IP stack. |
| One client, one request in flight | No pipelining and no concurrency, so every request is a full round trip. This is the baseline that "multiple clients" and "concurrent server" get compared against later. |
| `TCP_NODELAY` on both ends | Without it, Nagle's algorithm plus delayed ACKs add up to ~40 ms per round trip and would dominate every number here. |
| Per-request timing, always | Unlike the in-process benchmark, per-request clock reads are cheap *relative to what is measured*: a round trip is ~20 µs against a 41 ns tick, three orders of magnitude apart. **The percentiles here are real measurements, not tick counts.** |
| Commands built before timing | String construction is never timed. |
| 2 000-request warm-up, discarded | The first requests pay for TCP window ramp-up and a cold cache. |

### Design decisions, and why each one is there

| Decision | Reason |
| --- | --- |
| `std::chrono::steady_clock` | Monotonic. `system_clock` can jump backwards when NTP adjusts it, producing negative durations. `high_resolution_clock` is an alias for one of the two and which one is implementation-defined. |
| **Throughput and latency come from separate passes** | Throughput is measured by timing the whole sequence once and dividing — no per-call clock reads, so the number is free of measurement overhead. Percentiles need per-call brackets, which would inflate that total. The two numbers therefore come from two runs over the identical sequence. |
| **Clock overhead and tick are measured, not assumed** | Each is printed in the header. On this machine `now()` costs ~26 ns and the clock ticks every ~41 ns. Both are reported next to the percentiles because both are comparable to the operations being timed. |
| **Cache-aside fill on a miss** | How applications actually use a cache: a miss fetches from the backing store and populates the cache. Without it, only explicit `SET`s ever put anything in, so a read-heavy workload can never warm up and hit rate is capped by the *write fraction* rather than by capacity. The first version of Workload A lacked this and reported 25% hit rate at every capacity — measuring nothing. |
| **A full warm-up pass is discarded** | A cold cache misses on everything. Warming first means the numbers describe the steady state a long-running server lives in, and it moves one-off costs (the map's growth rehashes) out of the measured pass. |
| **Workload sequences are generated up front** | RNG cost is never measured, and the sequence is byte-identical between runs *and between cache configurations* — which is what makes the §2 A/B comparison valid at all. |
| **§2 is paired and order-alternated** | The two configurations are compared *within* each repeat, and which one runs first alternates. Comparing two independent medians let machine drift masquerade as a result; and always running A before B made B look consistently ~8% *faster* despite doing strictly more work, because the first run pays for heap growth the second reuses. |
| Skewed keys (80/20) for Workload A | Real traffic is skewed; that skew is the entire reason a cache works. Uniform access is the pessimistic case and is used for Workload B deliberately. |
| Shuffled access order in §1 | Walking keys in insertion order is unrealistically friendly to the prefetcher and overstates `GET` throughput. |
| Median of repeats | One unlucky run skews a mean badly and a median not at all. |
| Results accumulated into a printed checksum | Nothing observes a `get`'s return value otherwise, so `-O3` may delete the calls being measured. The checksum is printed, forcing the work to happen, and is identical across runs — independent evidence that the workload is deterministic. |
| Fixed-width 16-byte keys | Variable-length keys make hashing cost drift during a run. 16 bytes also fits small-string storage, so key handling costs no allocation. |
| Warns if `NDEBUG` is undefined | Benchmarking a Debug build measures the absence of the optimiser. |

### The limits of this harness — read before trusting a number

**Latency percentiles are quantised to the clock tick (~41 ns here).** A reported
`p50` of 42 ns means *one tick*, not a 42 ns measurement. For operations in the
40–400 ns range this is coarse, and the p50/p95/p99 should be read as tick counts
rather than fine-grained timings. Getting past this needs a different technique
(batching identical operations, or hardware counters) — ⬜ not yet scheduled.

**Absolute throughput is only comparable within a single sitting.** Across 12
invocations of an identical Stage 2 binary, the memory-latency-bound phases fell
into two clusters ~2× apart; the cause was never established, and CPU contention
alone did not explain it. The rule that follows:

> **Compare versions back to back in the same sitting, and trust ratios between
> phases over absolute throughput.**

Section 2 exists to satisfy that rule: it runs the A/B inside one process,
interleaved and order-alternated, rather than comparing against a number recorded
on another day.

### What is deliberately not measured yet

- ~~Memory footprint per entry~~ — ✅ measured in Stage 9: **221 bytes/entry**.
- ~~Concurrency scaling~~ — ✅ measured in Stages 6, 7 and 9.
- ~~TTL expiry cost~~ — ✅ measured in Stage 4 (below) and in the Stage 9 `ttl-heavy` workload.
- **Latency below the clock tick.** ⬜ Not yet measured; see above.
- **Cold-cache snapshot load.** ⬜ The persistence benchmark loads a file that was just written, so it never touches the disk.

### Observations on the TTL measurement (§6 of the harness)

This section is the one place where the same paired method produced a **clearly
resolvable** answer rather than "below the noise floor", which is worth dwelling
on because it is what makes the method credible.

- **The TTL check costs ~12% of a GET hit** (median +11.7% to +12.7% across five runs, middle half consistently +10% to +14%). In absolute terms ~12 ns on a ~104 ns operation — which is about what one `steady_clock::now()` read costs, and the clock read is exactly what the check adds.
- **Compare that to §2**, where the LRU capacity check straddled zero under the identical method. Two effects, one method, two different verdicts: that is evidence the harness can tell signal from noise rather than always shrugging.
- **The `optional` short-circuit is doing real work.** Keys *without* a TTL pay none of that 12%, because `has_value()` is tested before the clock is read. The cost is paid only by the keys that asked for it.
- **The latency percentiles cannot see this effect at all** — p50 reads 208 ns for both cases, because the 41 ns clock tick is three times larger than the 12 ns difference. The throughput measurement resolves what the percentile table cannot, which is a good illustration of why both are reported.
- **Walking a cache of entirely expired entries** runs at ~6.3 M ops/sec vs ~9.6 M for live hits. That path is not simply slower: those calls return nothing *and* delete an entry, so it is a different operation, not a penalty on the same one.

### Stage 3 findings (historical)

These came from the Stage 3 harness and still hold, but they are no longer the
latest results — for those see **Performance Evaluation** below and
`benchmarks/results/`.

- **Hit rate tracks capacity as LRU predicts** on skewed traffic: 3.3% at 1% capacity, 30.9% at 10%, 84.2% at 40%, 100% at full capacity. This is the clearest evidence in the project that eviction is choosing the *right* victims — a random-eviction policy would not produce this curve.
- **The cost of the capacity check is below the harness's noise floor.** The paired comparison straddles zero. That is the expected result for one predictable branch per insert, and it is explicitly *not* a claim that LRU is free — it means this experiment cannot resolve a cost that small.
- **Eviction-heavy churn is ~1.4× more expensive per request than hit-heavy traffic** (217 ns vs 152 ns). Every miss in Workload B costs a failed lookup, an insert, *and* an eviction.
- `GET hit` still costs ~6× `CONTAINS`, and the gap is still the splice plus the value copy — unchanged by Stage 3, as it should be.

---

## 14. Current Components

| Path | Purpose |
| --- | --- |
| `CMakeLists.txt` | Language standard, build options, warning flags, `compile_commands.json` link |
| `include/cachex/recency_list.hpp`, `src/recency_list.cpp` | `Entry` and `RecencyList` — the MRU→LRU order |
| `include/cachex/cache.hpp`, `src/cache.cpp` | `Cache`: single-threaded engine with capacity, eviction, TTL, `get_into`, `export_entries` |
| `include/cachex/sync_cache.hpp` | `SyncCache`: one mutex around a `Cache`; also serves as one shard |
| `include/cachex/sharded_cache.hpp`, `src/sharded_cache.cpp` | `ShardedCache`: N independently locked shards, same API |
| `include/cachex/persistence.hpp`, `src/persistence.cpp` | `PersistenceManager` (snapshot save/load) and `PeriodicSaver` |
| `include/cachex/line_buffer.hpp`, `src/net/line_buffer.cpp` | Byte stream → lines. No sockets |
| `include/cachex/protocol.hpp`, `src/net/protocol.cpp` | `Command`, `parse_command`, `reply_*` formatting |
| `include/cachex/command_handler.hpp`, `src/net/command_handler.cpp` | `execute(ShardedCache&, Command, PersistenceManager*)` — the bridge |
| `include/cachex/socket.hpp`, `src/net/socket.cpp` | RAII file-descriptor owner, `send_all`, `connect_to` |
| `include/cachex/connection.hpp`, `src/net/connection.cpp` | One client's read/dispatch/write loop |
| `include/cachex/server.hpp`, `src/net/server.cpp` | Listening socket, accept loop, worker threads |
| `src/server_main.cpp` | `cachex_server`: port, capacity, shards, snapshot path, auto-save interval |
| `src/client_main.cpp` | `cachex_client`: interactive CLI |
| `src/main.cpp` | `cachex`: in-process demo of LRU and TTL, no networking |
| `tests/` | 212 tests across 11 test files, plus `tests/test_framework.hpp` and `tests/test_main.cpp` |
| `benchmarks/*.cpp`, `benchmarks/bench_util.hpp` | The four benchmark binaries (§13) |
| `benchmarks/run_all.sh`, `benchmarks/results/` | Capture script and committed raw output |
| `benchmarks/RESULTS.md` | Per-stage benchmark write-ups |
| `docs/PROTOCOL.md` | Wire protocol specification |
| `.vscode/c_cpp_properties.json` | Shared editor config pointing IntelliSense at the compile database |

Tests by file: `cache_test`, `lru_test`, `ttl_test`, `recency_list_test`,
`line_buffer_test`, `protocol_test`, `server_test`, `concurrency_test`,
`sharding_test`, `persistence_test`, `version_test`.

### Build targets

```
cachex_warnings  (INTERFACE)  warning flags, carried as a target not global flags
        |
        +--> cachex_core  (static library, links Threads)
        |        cache · recency_list · sharded_cache · persistence · version
        |        (sync_cache is header-only)       KNOWS NOTHING ABOUT SOCKETS
        |        |
        |        +--> cachex_net  (static library)
        |        |        line_buffer · protocol · socket
        |        |        command_handler · connection · server
        |        |             |
        |        |             +--> cachex_server         the TCP server
        |        |             +--> cachex_client         interactive CLI
        |        |             +--> cachex_tests          unit + integration
        |        |             +--> cachex_net_bench      network benchmark   (opt-in)
        |        |
        |        +--> cachex                in-process demo
        |        +--> cachex_bench          in-process benchmark (opt-in)
        |        +--> cachex_persist_bench  snapshot benchmark   (opt-in)
        |        +--> cachex_bench_suite    full suite           (opt-in)
```

**The two libraries are the architectural boundary, expressed in the build
graph.** `cachex_core` does not link `cachex_net`, so cache code cannot start
depending on socket code by accident — it would fail to link. The dependency runs
one way: networking calls the cache.

**The library/executable split is the most important structural decision.** A
`main()` cannot be linked into a test binary — there would be two. If the logic
lived in `main.cpp` it would be untestable, and the usual workaround (tests
`#include`-ing the `.cpp`, or recompiling the sources) lets the two copies drift.
`cachex_core` means tests and benchmarks exercise *exactly* the object code the
server will run.

---

## 15. Design Decisions

### 15.1 Why C++17 rather than C++20

C++17 already contains everything this project needs, and Stage 3 leans on
`std::optional` twice — once for a lookup that may miss, once for a capacity that
may not exist. It is also available everywhere without effort — Apple Clang,
GCC 9+, MSVC 2019 — so anyone cloning this builds it with the compiler already on
their machine.

| C++20 feature | Why it is not needed |
| --- | --- |
| Coroutines | The one genuine temptation, for async networking. But an explicit `epoll`/`kqueue` loop makes the I/O model *visible*, and being able to explain readiness-based I/O is worth more than hiding it behind `co_await`. |
| Concepts / ranges | Real improvements to generic code. CacheX has almost no generic code. |
| `std::format` | Convenience only; library support is still uneven. |
| Heterogeneous lookup with `string_view` | ⚠️ The one real loss. `unordered_map<string, T>::find(string_view)` needs C++20's transparent hashing; in C++17 a lookup from a `string_view` must construct a temporary `std::string`. It has not bitten yet: the parser does tokenise into views, but each key is copied into a `std::string` in the `Command` before the lookup. Removing that copy would need a transparent hash functor, not a language upgrade. |

### 15.2 Why CMake

The de-facto standard for C++, so it is what reviewers expect and what IDEs,
`clangd`, sanitizers, and CI already understand. It makes Debug/Release a
configuration flag rather than hand-maintained compiler invocations. The CMake
here is **target-based**: properties attach to targets rather than directory-wide
globals, so include paths and flags travel with the target that needs them.

### 15.3 Why warnings are an INTERFACE target

`cachex_warnings` carries no code, only flags; targets opt in by linking it.
Appending to global `CMAKE_CXX_FLAGS` would apply them to any third-party library
added later, whose warnings are not ours to fix — and hundreds of them scrolling
past is how a real warning in our own code gets missed.

`-Werror` is available (`-DCACHEX_WARNINGS_AS_ERRORS=ON`) but off by default, so a
new compiler version with a new warning cannot break someone's clone.

### 15.4 Why the test framework is hand-written

~100 lines of header, no dependency. Only three capabilities are needed: register
a test, assert a condition, exit non-zero. GoogleTest or Catch2 would be the
largest thing in the repository and would add a fetch step between clone and
build. The macro names mirror Catch2's, so swapping later is contained.

One non-obvious detail: the test registry is a **function-local static**, not a
namespace-scope global. Tests in different translation units register themselves
before `main()`, and cross-translation-unit initialisation order is unspecified —
a namespace-scope registry could be registered *into* before it was constructed.
A function-local static is constructed on first use. This is the *static
initialisation order fiasco*.

### 15.5 Why the tests run under sanitizers

The correctness of this design rests on a claim about iterator validity, and
nothing in the type system enforces it. A dangling `std::list` iterator will
usually still *appear* to work, because the freed node's memory is typically
untouched. Tests alone would not catch it. Eviction (§5.3) adds a second
order-sensitive teardown, which doubles the reason.

```bash
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build/asan -j --target cachex_tests && ./build/asan/bin/cachex_tests

# macOS has no LeakSanitizer on arm64; the system leak checker fills the gap:
leaks --atExit -- ./build/debug/bin/cachex_tests
```

### 15.6 Why the version header is generated

`configure_file()` expands `include/cachex/version.hpp.in` into the build
directory, substituting the version from `project()`. Otherwise the version lives
in two places and drifts. The generated header goes in the build tree, never the
source tree.

### 15.7 Why `build/` is not committed

Build output is reproducible from the sources, specific to one compiler and one
machine, goes stale the instant a flag changes, and makes every diff unreadable.
Commit the inputs, never the outputs.

### 15.8 Why `compile_commands.json` is linked into the source root

CMake writes the compile database into each build directory, where editors do not
look. Without it the editor's C++ parser guessed a pre-C++17 standard and marked
every `std::optional` as an error in code that built cleanly. CMake now symlinks
the last-configured build's database into the source root, and a shared
`.vscode/c_cpp_properties.json` points at it; other personal editor settings stay
ignored.

### 15.9 Why raw benchmark output is committed, even though build output is not

It looks like it contradicts §15.7, and the difference is the point: build output
can be regenerated identically, a benchmark run cannot. Machine load, thermal
state and scheduling make every run different, so a number quoted in a document
is only checkable if the run that produced it is kept. `benchmarks/run_all.sh`
records the environment alongside the output, and all published figures are
medians of committed runs. Doing this exposed two quoted numbers that had come
from single, unsaved runs, and a warm-up bug in the suite itself.

---

## 16. Interview Questions I Should Be Able To Answer

### Why not use only a hash map?

A hash map has **no ordering**. It can tell you where a key is, but not which key
has gone longest without being touched. Finding the eviction victim would mean
examining every entry — O(n) per eviction, on a code path that runs on *every
insert* once the cache is full. The map is half the answer.

### Why not use only a linked list?

A linked list has **no search**. Finding a key means walking from the head: O(n)
per `get`, on the hottest path there is. It can maintain recency order perfectly,
but only if something else tells it which node to move. The list is the other
half.

### Why is LRU O(1) here?

Because the map stores **iterators into the list**, so the list is never
searched. Every operation is three constant-time steps: find the node via the map,
move or remove it via the list, keep the index in sync. The eviction candidate is
always the tail, reachable in O(1). See §5.1.

This only holds because `std::list` iterators stay valid across insertions and
erasures of other elements. That guarantee is what makes the stored handles
trustworthy for the entire lifetime of an entry.

### What happens on a GET?

Hash the key and look it up in the map. On a **miss**, return `nullopt` — nothing
is inserted, nothing is reordered. On a **hit**, splice the node to the head of
the list (making it most recently used), then return a *copy* of the value. It is
not a `const` method, because a read mutates the recency order. Full trace: §5.2.

### What happens when capacity is reached?

The insert goes through first: a new node at the head, a new index entry. Then,
because the size now exceeds capacity, the tail node — the least recently used —
is evicted: read its key out of the node, erase that key from the map, pop the
node off the list, increment the eviction counter. All O(1). Full trace: §5.3.

Note that an **update** never evicts, because the entry count does not change,
and an **erase** frees a slot so the next insert need not evict.

### What are the trade-offs of this design?

**In its favour:** every operation is O(1) average; the eviction is *exact* LRU
rather than an approximation; it is ~100 lines of straightforward code with no
dependencies; and the standard-library containers carry guarantees that are
documented and well understood.

**Against it:**

- **Memory overhead is high** — **221 bytes per entry, measured**, for ~80 bytes of payload (2.76×). Two node allocations per entry (list node and map node), plus the key stored twice, plus bucket overhead. Redis avoids much of this with *approximated* LRU: it samples a handful of random keys and evicts the oldest of the sample, which needs no list at all.
- **Poor cache locality.** Both containers are node-based, so a `get` chases pointers through scattered memory. The benchmark shows this clearly: `SET update` costs ~3.5× `SET insert` despite doing strictly less work, purely because of access patterns.
- **Every read is a write.** A `get` mutates the list. Since Stage 6 that is a real cost: readers cannot share a read-lock if they all need to reorder, which is exactly why LRU is hard to make concurrent and why sharding (Stage 7) matters.
- **O(1) is average, not worst case.** Adversarial keys colliding in one bucket degrade lookups to O(n). CacheX assumes a trusted network.
- **Exact LRU is scan-hostile.** One pass over a large key space evicts the entire hot set — a known LRU weakness that LRU-K, SLRU, or ARC address.

### Why is the key stored twice?

Eviction runs list → map: the list identifies the oldest node, but removing it
from the index needs its key. Without the key in the node, finding the index
entry would require scanning the map. The duplication is ~16–32 bytes per entry
and buys the O(1) eviction bound. §7 has the alternative and why it is deferred.

### Why doesn't `contains` update recency?

Deliberate: peeking is not using. If `EXISTS` refreshed a key's position, it
would become a way to keep dead data resident forever. It is also what allows
`contains` to be `const` while `get` cannot be.

### Does an update count as a use?

Yes — it moves the entry to the front, matching Redis, memcached, and the standard
LRU formulation. Writing to a key is at least as strong a signal of interest as
reading it. §4.6 has the full table and the reasoning.

### What's the worst case, and what would you do about it?

Hash collisions: adversarial keys landing in one bucket make every operation O(n).
The standard mitigation is a randomly seeded hash so an attacker cannot predict
bucket assignment. CacheX does not do this because it assumes a trusted network —
a decision that would have to be revisited the moment it faced untrusted input.

### How would you make this thread-safe?

A single mutex first, because it is obviously correct and gives a baseline to
measure against. A `shared_mutex` is the tempting next step but helps less than
expected — `get` *mutates* the list, so readers cannot genuinely share. The real
answer is **sharding**: N independent caches, key hashed to a shard, each with its
own lock. Contention drops roughly by a factor of N, exact LRU becomes per-shard
rather than global (an acceptable and standard trade), and nothing needs to be
lock-free. That is Stages 8–9.

### Why `steady_clock` and not `system_clock` for TTL?

`system_clock` can jump — NTP, an admin setting the date, a VM resuming from a
snapshot. A backwards jump keeps keys alive past their deadline; a forwards jump
mass-expires the whole cache at once and sends the next traffic burst straight to
the database. `steady_clock` is monotonic and measures elapsed time, which is what
a TTL actually is.

The cost: `steady_clock`'s epoch is unspecified (usually boot), so a deadline is
meaningless outside the process and cannot be serialised. Stage 8's snapshots
therefore store remaining durations plus a wall-clock save timestamp. §6.2, §10.2.

### Why lazy expiration, and what does it cost you?

Lazy means an expired entry is removed when an operation happens to encounter it —
the lookup has already found it and the iterator is in hand, so reclaiming costs
the same O(1) teardown as any removal. No background thread, no second index, no
coordination.

What it costs: an expired key that is never read again is **never reclaimed**. It
occupies memory, and worse, it occupies *capacity* — a bounded cache full of
expired corpses will evict a live entry to make room, because eviction picks the
LRU tail and does not know which entries are already dead. `size()` is therefore
an upper bound on live keys, not a count. §6.4.

### Why no background cleanup thread?

When TTL was added there were three reasons, in order: (1) the cache was not
thread-safe, so a sweeper mutating the map while a caller held an iterator would
have been a data race and a use-after-free — and the bug would have presented as a
TTL bug. (2) The right sampling policy needs measurements; Redis's "20 keys,
repeat while >25% expired" are tuned constants, not first principles.
(3) Sampling a random key from an `unordered_map` is not O(1), so active expiry
needs a data structure the cache does not have.

Reason 1 is gone — locking arrived in Stage 6 and sharding in Stage 7, so a sweep
can now run per shard under that shard's lock. Reasons 2 and 3 remain, which is
why active expiry is still future work (Stage 10). §6.5.

### What happens if I `SET` a key that already has a TTL?

The TTL is cleared — a set replaces the whole entry, expiry included. That is
Redis's `SET` default too; Redis needs `KEEPTTL` to preserve it. One rule, no
hidden state carried across a write.

### What does a TTL of zero do?

It erases the key and stores nothing. An entry that can never be read is pure
cost: it would occupy capacity and could evict a live entry. The invariant is
*after `set(k, v, ttl)`, the key is visible iff `ttl > 0`*. Redis differs here —
it rejects `SET ... EX 0` as an error, and uses `EXPIRE key 0` for the delete.

### How much does the TTL check cost?

~12% of a GET hit, measured — about 12 ns, which is one `steady_clock::now()`
read. Keys *without* a TTL pay none of it, because the `optional` is tested
before the clock is read. That short-circuit is the whole reason TTL is close to
free for keys that do not use it. §13.

### How would you test something that depends on time without flaky tests?

The asymmetry is the trick: a sleep can overshoot but never undershoot. So
"expired by now" assertions are safe with a modest wait, because system load only
makes them more true. The fragile direction is "still alive", which is why those
tests use a TTL (2 s) far larger than any plausible stall, while the waits stay
short (130 ms). 20 consecutive runs of the suite produced no flakes.

The alternative is an injectable clock, which makes the tests instant and exactly
deterministic. It is not used here because it puts indirection in the hottest
path in the cache to serve the tests — worth revisiting if the ~1.7 s of sleeping
becomes annoying, but not worth it yet.

### TCP or UDP for a cache, and why?

TCP. A client needs to know its `SET` arrived, arrived once, and arrived intact,
and a reply larger than one packet must be reassembled in order. TCP gives all of
that — reliability, ordering, de-duplication, retransmission, flow control. Over
UDP you would rebuild every one of those by hand and end up with a worse TCP.

UDP wins when *late data is worthless*: live audio, video, telemetry, where
re-sending a packet that is already too old only makes things worse. A cache is
the opposite — a slightly late answer is still the right answer. (memcached does
offer a UDP mode for `get`-only traffic, where a lost reply can just be
re-fetched.)

### What is the difference between `listen()` and `accept()`?

`listen()` is a one-time state change: it marks the socket *passive* and tells
the kernel to complete TCP handshakes on your behalf and queue the finished
connections, up to `backlog` deep. It does not block and it accepts nothing.

`accept()` takes one already-completed connection off that queue and returns **a
new socket** for it. The listening socket stays open and is never read from or
written to — its only job is producing new sockets.

The consequence people miss: a client's `connect()` can succeed while the server
is busy and has not called `accept()` at all, because the kernel did the
handshake. "Connected" does not mean "being served".

### What happens during `connect()`?

The client's kernel sends SYN, the server's kernel replies SYN-ACK, the client
ACKs. The server *application* runs no code during any of this — its kernel
completes the handshake and puts the connection on the accept queue. `connect()`
returns once the handshake is done; if the backlog is full, it may block or be
refused.

### Why can `recv()` return partial data?

Because TCP is a byte **stream**, not a message queue. It guarantees the bytes
arrive in order; it never promised to preserve the boundaries between the sender's
writes. One `recv()` can return half a command, exactly one, three, or two and a
half — depending on packet sizes, MTU, Nagle, and how the sender's buffer
flushed. None of those is an error.

A server that assumes "one `recv()` = one request" passes casual testing against
a hand-typed client and corrupts itself the moment a real client pipelines.

The same applies on the way out: **`send()` may accept fewer bytes than offered**,
so it has to loop too. Ignoring `send()`'s return value truncates replies and
corrupts the peer's stream.

### Why is protocol framing necessary?

Because the transport deliberately does not provide it. TCP delivers
`SET foo bar` faithfully and has no idea those bytes form a command. Framing is
the application protocol's entire job: putting the boundaries back.

CacheX frames with a newline. RESP frames with a length prefix (`$3\r\nfoo\r\n`),
which is strictly better: the length comes first, so the payload needs no escaping
and can contain any byte — including spaces and newlines. That is exactly the
limitation CacheX v1 has, and it is documented rather than hidden
(`docs/PROTOCOL.md` §3).

### What happens when a client disconnects?

`recv()` returns **0**. That is end-of-stream — not an error, and not "no data
yet" (which would be `-1` with `EAGAIN` on a non-blocking socket). The server
closes its side and returns to `accept()`.

An *abrupt* disconnect is nastier: the server may be mid-`send()` to a socket the
peer has already abandoned. That raises `SIGPIPE`, whose default action is to
**kill the process** — a server dying because a client hung up. It is suppressed
with `MSG_NOSIGNAL` (Linux) or `SO_NOSIGPIPE` (macOS/BSD), and the failed `send()`
is then just an error code. There is a test that hangs up on the server
mid-reply five times and then checks the server still answers.

### Why does the server need `SO_REUSEADDR`?

When a server exits, its closed connections sit in `TIME_WAIT` for up to a couple
of minutes, so that late duplicate packets cannot be delivered to a new
connection reusing the same port pair. Without `SO_REUSEADDR`, restarting the
server fails with "Address already in use" for that whole window.

### Why is the cache kept ignorant of sockets?

So that the request/response behaviour can be tested without a socket, and so the
transport can be replaced without touching the engine. `execute(Cache&, Command)`
takes a parsed command and returns a reply string — the whole server's semantics
are testable in-process.

It is enforced by the build graph, not discipline: `cachex_core` does not link
`cachex_net`, so cache code that reached for a socket would fail to link.

### Race condition vs deadlock?

A **race condition** is unsynchronised concurrent access where the result depends
on timing — two threads splicing the same list node, and the outcome is a
corrupted list. The program does too little synchronisation.

A **deadlock** is two or more threads each waiting for a lock the other holds, so
none can proceed. The program does too much synchronisation, in the wrong order.

They pull in opposite directions, which is why fixing one carelessly causes the
other. CacheX avoids deadlock structurally: there are two mutexes
(`SyncCache::mutex_` and `Server::workers_mutex_`) and **they are never held at
the same time**. Two locks that cannot be held together cannot deadlock, and that
is much easier to verify than a lock-ordering rule.

### Mutex vs atomic?

An **atomic** makes a single operation on a single variable indivisible, with no
blocking. Use it for a counter or a flag — `connections_served_`,
`stop_requested_`.

A **mutex** protects an arbitrary *region of code* touching arbitrary state. Use
it when an invariant spans several variables. That is exactly the cache: a map
and a list that must agree with each other. No number of atomics would help,
because the invariant is between structures, not inside one word. Atomics are
cheaper; mutexes are more general.

### Why does one global mutex become a bottleneck?

Because it makes the whole cache a **sequential section**: exactly one thread may
be inside it, so the cache's throughput is capped at what one core can do
regardless of how many threads exist. Amdahl's law then bounds the whole system.

The measurement is blunt: N threads calling `SyncCache::get()` directly scale
1.00x → 0.83x → 0.48x at 1, 2 and 4 threads. Adding threads makes it **slower**,
because they queue and pay handoff cost on top.

The nuance worth knowing — and the thing the benchmark's control proves — is that
*over TCP this is not yet the limit*. Lock-free `PING` plateaus at the same place
as `GET`, because a ~20 µs round trip dwarfs a ~0.4 µs cache operation. The mutex
is a wall the system will hit once the transport stops being the bottleneck, not
one it is hitting today. §8.6.

### Why does LRU make GET a write operation?

Because "least recently used" has to be maintained, and the only moment the cache
learns a key was used is when someone reads it. So `get()` splices that entry to
the head of the recency list — and if it has expired, deletes it from both
structures. Two concurrent "reads" are two concurrent list mutations.

That is why `get()` takes an **exclusive** lock, and why `std::shared_mutex` is a
trap here: readers can only share a lock if they are genuinely read-only, and in
a cache the common operation is a reader that writes.

### What happens with 100 concurrent clients?

With thread-per-connection: 100 worker threads, each with its own stack,
competing for 12 hardware threads. They are accepted — the default
`max_connections` is 256 — and the 257th would be refused with an error rather
than allowed to exhaust memory.

What the measurements predict: **throughput stays flat at roughly the 8-client
figure (~120k req/sec) while latency grows in proportion.** That is what the
1→16 client data already shows (p50 20 µs → 128 µs while throughput plateaus),
and it is the signature of a saturated resource: the queue grows, the service
rate does not. Clients experience it as latency, not as errors.

Beyond a few hundred, thread-per-connection stops being viable at all — context
switching and stack memory dominate. The answer is an event loop (a handful of
threads multiplexing many sockets with `epoll`/`kqueue`), and separately sharding
the cache so the lock stops being one sequential section.

### How does sharding reduce lock contention?

One mutex makes the cache a single queue: every operation waits behind every
other one, whatever key it touches. N shards make it N independent queues, each
with its own mutex, map and LRU list. Two threads collide only when their keys
hash to the same shard — roughly 1/N of the time for well-distributed keys.

Measured in isolation: 4 threads calling `get()` went from 2.25M ops/sec with 1
shard to 7.48M with 8 shards, **+232%**.

### When does sharding NOT help?

Four cases, three of them measured here:

1. **No contention.** At 1 thread it is +0.0% — a hash and an indirection bought in exchange for waiting that never happened.
2. **Something else is the bottleneck.** Over TCP, sharding moved throughput by ≤0.8%, because a ~20 µs round trip dwarfs a ~0.4 µs cache operation. Optimising a non-binding constraint changes nothing end to end, however good the microbenchmark looks.
3. **One hot key.** All its traffic lands on one shard regardless of shard count.
4. **A globally consistent view is needed.** `size()` locks shards one at a time, so it is approximate under load; making it exact would reintroduce the global lock.

### What does sharding cost in correctness?

LRU becomes per-shard rather than global: a hot key in a crowded shard can be
evicted while a colder key in a quiet shard survives. Measured on a skewed
workload, that cost **≤0.02 percentage points** of hit rate — effectively free,
because the hash spreads keys evenly enough that every shard sees a similar slice
of the distribution. The honest claim is "cheap when keys distribute evenly", not
"free".

`size()` also stops being a consistent snapshot, for the same reason.

### Why not just hash the key to pick the shard?

Because the shard's own `unordered_map` already hashes the key with `std::hash`
to choose a bucket. If the shard index used the same value, every key in a shard
would share `h % shard_count` — which correlates with the bucket index whenever
the shard count and bucket count share a factor, clustering keys into a few
buckets of each map. Running the hash through a mixing step (splitmix64's
finalizer) decorrelates the two uses. A test checks 8000 keys land within ±25% of
even across 8 shards.

### How many shards should you use?

It is a tuning knob, not a constant to derive. More shards means less contention
but more memory overhead (each carries its own map, list and mutex) and finer
capacity granularity — and with total capacity below the shard count, shards
would get zero capacity, which `ShardedCache` prevents by reducing the count.

The measured answer here: the win was monotonic from 1 to 8 shards at 4 threads,
so 8 was not yet the point of diminishing returns for this workload. A sensible
default is a small multiple of the core count.

### Why does the snapshot store remaining TTL and a timestamp, rather than deadlines?

Because TTLs are tracked on `steady_clock`, whose epoch is unspecified — a
deadline from one process means nothing in the next. So the file stores
*durations*.

A duration alone is not enough either: a key saved with 60 ms left would come
back with a fresh 60 ms however long the server was down. The header therefore
carries a `system_clock` timestamp, used for exactly one thing — measuring how
long the file sat on disk — and the loader subtracts it. Elapsed time is clamped
at zero so a backwards clock jump can never *extend* a TTL.

This was found by a test, not by inspection. The first version had the bug.

### How do you take a snapshot without corrupting it while clients are writing?

Two questions that are worth separating.

**Well-formed output:** each shard is locked, copied, unlocked — so every record
written is a whole record. The file is never open while a lock is held, so a slow
disk cannot stall the request path.

**A single point in time:** it is *not* one. Shards are locked one at a time, so
shard 0 is read slightly before shard N-1. Locking all of them at once would give
a true instant and would reintroduce the global stall sharding exists to remove.
That is a deliberate trade, and it is the same one `size()` makes.

The write itself is atomic: temp file, then `rename()`, which POSIX guarantees is
atomic. An interrupted save leaves the old snapshot or the new one, never a
truncated file.

### What does CacheX persistence NOT give you?

Durability, mainly. `fsync` is never called, so a power loss can lose a snapshot
that `save()` reported as successful. Everything written since the last save is
gone on a crash — there is no write-ahead log. Saves are full rewrites, O(n)
regardless of how little changed. There is no checksum, so silent corruption is
not detected, and no replication or compaction.

It makes a **cache** survive a *planned* restart. It does not make CacheX a
system of record, and the data must always be reconstructible from somewhere
else. §10.7 has the full comparison.

### Why do SAVE and LOAD take no arguments?

Because the snapshot path is server configuration, never something a client
supplies. Accepting a path over the network would let any client read or
overwrite an arbitrary file the server process can reach — a path-traversal hole
handed over for free. The server is configured with one path; clients can only
ask it to use that one.

### How would you test that eviction is correct?

Not by checking `size()` — that passes even if the *wrong* entry is evicted. The
tests assert the full recency order as a string (`"d,c,b"`), so a failure shows
the whole state rather than just which key went missing. They cover the boundary
capacities (0, 1, exactly full), each operation's effect on recency, and a churn
test that verifies the map and the list still agree on which keys exist.

---

## 17. Future Roadmap

Stages 1–9 are done; the table keeps them for the record.

| # | Stage | What it adds | Core concepts | Status |
| --- | --- | --- | --- | --- |
| 1 | Project foundation | CMake, structure, warnings, test harness | Build systems, testability | ✅ |
| 2 | Core cache | `set`/`get`/`erase`/`contains`/`size`, hash map + list | Hash tables, iterator invalidation, ownership | ✅ |
| 3 | LRU eviction | Capacity limit, O(1) eviction, workload benchmarks | Why O(1) LRU needs both structures; hit rate | ✅ |
| 4 | TTL | Per-key expiry deadlines, lazy expiration | Lazy vs. active expiry; `steady_clock` vs. `system_clock` | ✅ |
| 5 | TCP server + protocol | `socket`/`bind`/`listen`/`accept`, line protocol, CLI client, network benchmark | The socket API; framing; partial reads and writes | ✅ |
| 6 | Concurrency | Thread-per-connection, `SyncCache`, scaling benchmark | Data races, mutexes, contention, why `shared_mutex` disappoints for LRU | ✅ |
| 7 | Sharded cache | N independently locked shards, A/B/C/D benchmark matrix | Lock contention; when optimising the wrong thing changes nothing | ✅ |
| 8 | Persistence | Snapshot to disk, restore on startup, `SAVE`/`LOAD` | Serialisation; atomic replace; what a cache is *not* | ✅ |
| 9 | Benchmark suite + profiling | Standard workloads, full metrics, `sample` profile, `get_into` (+45%), committed results | Measuring before optimising; what not to claim | ✅ |
| — | Event-loop I/O | `kqueue`/`epoll` loop serving many connections from a few threads | Readiness-based I/O; why thread-per-connection stops scaling | ⬜ next — over TCP the transport, not the cache, is the measured limit |
| 10 | Active expiry | Per-shard background sweep | Sampling policies; bounding sweep cost under a shard lock | ⬜ |
| 11 | Further optimisation | Remove the duplicated key, reduce the 221 bytes/entry, re-benchmark against committed results | Cache locality, allocation cost, proving an improvement | ⬜ |

Stage numbers were renumbered as the plan changed; earlier write-ups in
`benchmarks/RESULTS.md` sometimes refer to concurrency as "Stage 7" or "Stage 8".
The ordering is what matters, not the number.

Each stage ends with this document updated: components in §14, decisions in §15,
new questions in §16, and the diagram in §3 grown to match what actually exists.

---

# Performance Evaluation

Everything in this section is output from `cachex_bench_suite`, `cachex_net_bench`
and `cachex_persist_bench`. Nothing is estimated, and where a figure varies
between runs the range is given rather than the best one.

## Environment

| | |
| --- | --- |
| Machine | Apple M2 Pro, 12 cores / 12 hardware threads, 16 GB RAM |
| OS | macOS 26.5.2 (arm64) |
| Compiler | AppleClang 21.0.0 |
| Build | `-O3 -DNDEBUG`, C++17, `-Werror` with the full warning set |
| Clock | `steady_clock` — measured overhead ~29 ns/read, **tick 41 ns** |
| Date | 2026-09-11 |

The machine is a laptop, not an isolated benchmark host. Client and server share
the same 12 hardware threads in the networked runs, and the OS is not quiesced.

## Versions compared

| | Implementation | Locking |
| --- | --- | --- |
| **A — baseline** | `Cache` | none; single-threaded by construction |
| **B — concurrent** | `SyncCache` | one global `std::mutex` |
| **C — optimised** | `ShardedCache(8)` | 8 independently locked shards |

All three expose the same operations, so the suite runs the identical workload
against each.

## Workload definitions

400,000 operations each, generated once from seed `20260911` and replayed
identically by every version. "Skewed" means 80% of traffic to 20% of the keys;
cache-aside means a `GET` miss populates the entry.

| Workload | Mix | Keys | Capacity | Notes |
| --- | --- | --- | --- | --- |
| `read-heavy` | 90% GET / 10% SET | 100,000 skewed | 40,000 | The common cache shape |
| `balanced` | 50% GET / 50% SET | 100,000 skewed | 40,000 | |
| `write-heavy` | 10% GET / 90% SET | 100,000 skewed | 40,000 | |
| `high-churn` | 20% GET / 80% SET | 200,000 uniform | 20,000 | 10× capacity, no hot set — worst case for a cache |
| `ttl-heavy` | 70% GET / 30% SET | 100,000 skewed | 40,000 | Every SET carries a 2 s TTL |

Thread counts 1 / 2 / 4 / 8 / 16 for B and C; A is single-threaded.

## Methodology

- **One generated sequence per workload**, replayed by every version — a difference between rows is a difference in the cache, not in what was asked of it.
- **All threads released by a start gate**, so an N-thread run genuinely has N threads in flight rather than a staggered ramp.
- **Throughput from wall time** of the whole run, so it is not limited by clock resolution. **Latency percentiles are quantised to the 41 ns tick**; operations here take 130–900 ns, so p50/p95/p99 are accurate to roughly one tick and sub-tick differences are invisible.
- **Hit ratio, eviction count and error count are exact** and identical across versions for a given workload (≈78.95% and ≈44,200 evictions for `read-heavy`), which is the check that the workloads really are identical.
- **Every figure below is the median of 5 captured runs**, with the range given where it is wide. The raw output of those runs is committed in `benchmarks/results/`, alongside `environment.txt` recording the machine, commit and load average. `benchmarks/run_all.sh` regenerates all of it.
- **A discarded warm-up pass runs before each workload.** Without it, whichever version ran first absorbed the cost of faulting in that workload's pages and growing the allocator — and that was always A, which made the *unlocked* baseline measure slower than the mutex version at one thread. An impossible result, and purely an artefact of ordering. It was caught by noticing that A < B at one thread, which cannot happen when B does strictly more work.
- Zero errors in every configuration reported here.

## Results — `read-heavy` (representative)

| version | threads | ops/sec (median) | range | p50 | p95 | p99 | hit % |
| --- | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| A baseline | 1 | **6,039,943** | 4.62 M – 6.09 M | 125 | 333 | 375 | 78.95 |
| B mutex | 1 | 5,049,065 | 4.09 M – 5.67 M | 125 | 375 | 541 | 78.95 |
| B mutex | 2 | 2,417,427 | 2.01 M – 2.51 M | 167 | 2,584 | 14,916 | 78.96 |
| B mutex | 4 | 1,079,059 | 1.02 M – 1.20 M | 375 | 19,541 | 46,458 | 78.98 |
| B mutex | 8 | 1,657,196 | 1.49 M – 1.89 M | 292 | 15,625 | 50,542 | 78.96 |
| B mutex | 16 | 1,880,650 | 1.69 M – 2.02 M | 250 | 14,875 | 91,625 | 78.94 |
| C sharded | 1 | 5,091,112 | 3.71 M – 5.17 M | 125 | 375 | 459 | 78.96 |
| C sharded | 2 | 3,226,120 | 2.82 M – 3.54 M | 250 | 3,167 | 5,750 | 78.96 |
| C sharded | 4 | 3,152,839 | 2.97 M – 3.43 M | 333 | 6,208 | 12,750 | 78.98 |
| C sharded | 8 | 2,956,268 | 2.81 M – 3.23 M | 417 | 14,917 | 27,834 | 78.97 |
| C sharded | 16 | 2,765,883 | 2.74 M – 2.91 M | 500 | 34,833 | 62,542 | 78.91 |

Zero errors and ~44,200 evictions in every row. Hit rate spans 78.91–78.98%
across all eleven configurations and all five runs — the check that every version
really did receive the identical workload.

The other four workloads follow the same shape; the full tables are the suite's
own output.

### C vs B at matched thread counts — throughput

| workload | 1 thread | 2 threads | 4 threads | 8 threads | 16 threads |
| --- | ---: | ---: | ---: | ---: | ---: |
| read-heavy | −8.8% | +28.6% | **+174.9%** | +69.3% | +46.9% |
| balanced | −13.1% | +16.3% | **+197.0%** | +56.2% | +57.1% |
| write-heavy | −8.3% | +22.1% | **+178.7%** | +94.1% | +67.0% |
| high-churn | −11.0% | +29.1% | **+197.7%** | +67.1% | +35.9% |
| ttl-heavy | −4.3% | +44.5% | **+234.6%** | +66.4% | +57.0% |

### C vs B at matched thread counts — p99 latency (negative is better)

| workload | 1 thread | 2 threads | 4 threads | 8 threads | 16 threads |
| --- | ---: | ---: | ---: | ---: | ---: |
| read-heavy | +9.8% | −59.5% | **−69.3%** | −36.7% | −33.1% |
| balanced | +13.3% | −55.8% | **−72.7%** | −32.8% | −46.8% |
| write-heavy | +9.8% | −60.4% | **−69.6%** | −54.4% | −68.8% |
| high-churn | +13.3% | −66.8% | **−75.2%** | −49.5% | −62.2% |
| ttl-heavy | +9.2% | −62.1% | **−76.1%** | −38.6% | −56.4% |

## The result that matters most, and it is not flattering

**No multi-threaded configuration beats the single-threaded baseline.** A at one
thread (6.04 M ops/sec median on `read-heavy`) is the fastest row in the table. Every
version gets *slower* as threads are added.

That is not a bug, and it is not fixed by better locking. A cache operation here
costs ~167 ns and touches shared memory. Coordinating threads — lock handoff,
cache-line transfer between cores, scheduling — costs more than the work being
parallelised. Sharding reduces that cost substantially but does not turn it
positive.

So the honest framing of sharding's value is:

- **Against the single global mutex at the same thread count, it is decisively better** — +175% to +235% throughput and −69% to −76% p99 at 4 threads.
- **At one thread it is 4–13% slower**, because it adds a hash and an indirection and removes contention that was not there.
- **It does not make the cache scale.** Nothing here does.

The system-level reason threads exist at all is not cache throughput — it is that
a server must serve many connections, and Stage 5 measured that a TCP round trip
(~20 µs) dwarfs a cache operation (~0.4 µs). Over TCP the cache is ~2% of a
request, which is why the networked benchmark shows sharding moving throughput by
≤0.8% (Stage 7 section of `benchmarks/RESULTS.md`).

## Memory

Measured as `malloc` bytes-in-use before and after building a cache — not RSS.
The first version of this measurement used RSS and reported **0 bytes/entry** for
100,000 entries, because the allocator had already grown the heap and never
returned the pages.

| entries | heap delta | bytes/entry | vs payload |
| ---: | ---: | ---: | ---: |
| 100,000 | 20.6 MiB | 216.4 | 2.70× |
| 250,000 | 52.7 MiB | 221.2 | 2.76× |
| 500,000 | 105.5 MiB | 221.2 | 2.76× |

**221 bytes per entry** for an 80-byte payload (16-byte key + 64-byte value).

This **corrects an estimate carried since Stage 3**, where the layout suggested
"~180–200 bytes/entry". The real figure is higher. The difference is the
allocator's per-allocation bookkeeping across the two nodes per entry plus the
value's buffer — which is exactly the sort of thing a layout calculation misses.

## Profiling

Tool: macOS `sample` (8 s of the running suite). `perf` and `valgrind` are not
available on this platform; `dtrace` requires privileges the benchmark does not
have; `/usr/bin/time -l` supplied instruction counts as a cross-check.

Heaviest stacks:

| samples | symbol | what it is |
| ---: | --- | --- |
| 7,852 | `__psynch_mutexdrop` | mutex handoff, in the kernel |
| 5,875 | `__ulock_wait` | threads blocked on a mutex |
| 1,270 | `mach_continuous_time` | **the benchmark's own per-operation clock reads** |
| ~700 | `__hash_table::find` | the actual lookup |
| 639 + 456 | `_xzm_free`, `_xzm_xzone_malloc_tiny` | allocation and free |

Three conclusions:

1. **Lock contention dominates by an order of magnitude** (~13,700 samples). This confirms that Stage 7's sharding work targeted the right thing, and it explains why B degrades so sharply with threads.
2. **The benchmark's own instrumentation is the third-largest cost.** Per-operation timing is not free; this is the same 41 ns tick showing up as real overhead, and it is a reason to trust the wall-clock throughput figures over the percentiles.
3. **Allocation is the largest remaining cost inside the cache itself.** That one is actionable.

## Optimisation: `get_into()`

**The bottleneck.** `get()` returns `std::optional<std::string>`, so every hit
constructs a fresh string. For values past the small-string limit — 64 bytes here
— that is one `malloc` and one `free` per lookup. On `read-heavy` (90% GET, ~79%
hit rate) that is roughly 0.71 allocation/free pairs per operation, purely to
hand back a copy the caller usually discards immediately.

**The change.** An additional overload that copies into a caller-owned buffer:

```cpp
bool get_into(const std::string& key, std::string& out);
```

`out.assign(...)` reuses the buffer's existing capacity, so a caller that keeps
one buffer allocates once rather than once per hit. Semantics are otherwise
identical to `get()` — it counts as a use, and it reclaims expired entries. The
existing `get()` is unchanged, so nothing about the safety argument for returning
a copy (§12) is weakened.

**Before / after** — same workload, same cache, single-threaded so no contention
is mixed in:

| variant | ops/sec (median of 5) | range | avg ns | p50 |
| --- | ---: | --- | ---: | ---: |
| `get()` | 5,118,081 | 4.47 M – 5.41 M | 195.4 | 125 |
| `get_into()` | **7,634,589** | 6.05 M – 7.85 M | 131.0 | 83 |

**Median +45.3% throughput**, avg latency 195 ns → 131 ns, p50 125 ns → 83 ns.

The per-run improvement ranged from **+18.2% to +72.9%** across the five captured
runs. That spread is machine noise, not a property of the change: the two
variants are measured back to back, and when the machine is busy the `get()`
baseline sinks further than `get_into()` does, inflating the ratio.

**The claim worth making is the median, +45%, with +18% as the floor actually
observed.** An earlier draft of this section quoted +46% from a single
unrecorded run — a number inside the range, but not reproducible from anything
committed. Every figure here now comes from `benchmarks/results/bench_suite.*.txt`.

**Why it is not wired into the server.** The measurements say it would not show
up there. A TCP round trip is ~20 µs against a ~0.4 µs cache operation, so
removing ~58 ns from the cache path moves an end-to-end request by ~0.3%. Adding
it would be optimising a component that is not the constraint — the exact mistake
Stage 7's data was collected to prevent. It is exposed as public API for
in-process users, tested, and left there.

## Limitations

- **Laptop, not an isolated host.** Numbers drift with machine state; run-to-run variation on the memory-bound phases has been observed at up to 2× in earlier stages.
- **Latency percentiles are tick-quantised** at 41 ns. Sub-tick effects — such as the 12 ns TTL check measured in Stage 4 — are invisible in p50/p95/p99 and only show up in throughput.
- **Per-operation timing is itself ~7% of the profile.** The instrumented runs are measurably slower than uninstrumented ones.
- **Synthetic key distributions.** 80/20 and uniform. Real traffic is Zipfian with a hot set that moves over time; none of these workloads model a shifting working set.
- **One key size and one value size** (16 B / 64 B). The `get_into()` result in particular depends on the value exceeding small-string capacity; for short values there would be no allocation to remove.
- **Memory figures exclude allocator slack** within a size class, so the true cost to the process is somewhat higher than 221 bytes/entry.
- **Networked results are loopback**, with client and server on one machine sharing 12 hardware threads.

---

# Resume-Ready Metrics

Only figures that were actually measured. Each is the **median of 5 captured
runs** unless stated otherwise, and the raw output is committed in
`benchmarks/results/` — every number here can be found in a file. Regenerate with
`benchmarks/run_all.sh`.

**Throughput**

- **6.0 M operations/sec** single-threaded on a 90/10 read-heavy workload (400k ops, 100k keys, 40k capacity, 78.95% hit rate) — `cachex_bench_suite`
- **3.2 M operations/sec** at 4 threads with an 8-shard cache on the same workload
- **~48,000 requests/sec** end-to-end over TCP with one client, **~123,000 req/sec** at 16 concurrent clients — `cachex_net_bench`

**Improvement from sharding** (8 shards vs one global mutex — same workload, same thread count)

- **+175% to +235% throughput at 4 threads** across five workloads
- **+56% to +94% throughput at 8 threads**
- **−69% to −76% p99 latency at 4 threads**
- **−33% to −54% p99 latency at 8 threads**
- **+232% throughput at 4 threads** on a pure `get()` microbenchmark (2.25 M → 7.48 M ops/sec) — `cachex_net_bench`

**Improvement from profiling-driven optimisation**

- **+45% throughput (median; +18% worst observed)** from eliminating a per-hit heap allocation on the read path, identified by stack profiling — average latency 195 ns → 131 ns, p50 125 ns → 83 ns — `cachex_bench_suite`

**Latency**

- **p50 125 ns / p95 333 ns / p99 375 ns** single-threaded, read-heavy
- **p50 ~20 µs / p99 ~30 µs** end-to-end over TCP, one client
- **p99 62 µs at 16 concurrent clients**, sharded

**Cache behaviour**

- **78.95% hit rate** on an 80/20 skewed workload at 40% capacity — held to 78.91–78.98% across every version, thread count and run
- **84.21% hit rate at 40% capacity vs 3.32% at 1%** on the LRU capacity sweep — LRU retaining the working set
- **9.65% hit rate** under high churn (uniform keys, 10× capacity) — the honest floor when there is no locality to exploit
- **0.02 percentage points** — the hit-rate cost of per-shard LRU versus global LRU
- **Zero errors** across every benchmark configuration

**Resource use**

- **221 bytes/entry** measured (2.76× the 80-byte payload) — identical in all 5 runs
- **90 bytes/entry** on disk in a snapshot — ~2.5× smaller than in memory
- **264 ms to save / 252 ms to load 500,000 entries** (42.9 MiB), scaling linearly at ~0.5 µs/entry

**Scaling — stated honestly**

- **2.6× throughput from 1 to 16 concurrent TCP clients**, limited by the transport rather than the cache (a lock-free `PING` control plateaus at the same point)
- **In-process, concurrency does not improve throughput at all** on this hardware: the single-threaded baseline is the fastest configuration measured. Sharding's measured value is beating the global mutex under concurrency, not scaling past one thread.

**A note on how these were arrived at**

Two measurement bugs were found and fixed while producing them, both by noticing
an impossible result rather than by inspection:

- Memory was first measured with RSS, which does not shrink when memory is freed; it reported **0 bytes/entry** for 100k entries. Switched to `malloc` bytes-in-use.
- The suite measured version A first in each workload with no warm-up, so A absorbed the page-fault and allocator cost — making the *unlocked* baseline appear slower than the mutex version at one thread, which cannot be true.

Single-run point values have been replaced throughout by medians over five
captured runs, because this machine's memory-bound phases have been observed to
swing by up to 2× between runs.
