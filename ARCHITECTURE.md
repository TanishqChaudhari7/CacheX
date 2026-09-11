# CacheX — Architecture

> Living document. It is updated at every stage of the project.
> **Current stage: 4 — TTL (lazy expiration).**

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
| Active expiry | Background sweep of expired keys | ⬜ after Stage 8 |
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
- **Approximated LRU.** Redis samples a few random keys and evicts the oldest of the sample, trading exactness for a smaller per-entry footprint. CacheX implements *exact* LRU because the point here is to demonstrate that the O(1) structure works, not to save 16 bytes per key.

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
        +-------------------------------------------+
        |                  Cache                    |  cache.hpp / cache.cpp
        |   set · set+ttl · get · ttl · erase       |
        |   contains · size · capacity · evictions  |
        |   expired_removals                        |
        +-------------------------------------------+
             |                            |
             |  owns                      |  borrows (iterators)
             v                            v
   +----------------------+   +------------------------------+
   |     RecencyList      |<--|            index_            |
   |   std::list<Entry>   |   |  unordered_map<              |
   |                      |   |    string,                   |
   |  head ......... tail |   |    RecencyList::Iterator>    |
   |  (MRU)        (LRU)  |   +------------------------------+
   +----------------------+     "where is this key?"   O(1)
     "what is oldest?"  O(1)
             |
             +--> ✅ LRU eviction: oldest() / pop_oldest()
             +--> ✅ TTL: Entry::expires_at, checked lazily on access
             +--> ⬜ active expiry sweep (needs thread safety first, Stage 8)
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
- It is node-based: every element is a separate allocation, and traversal chases pointers. This shows up clearly in the benchmark (§9).
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
for keys that do not use it — and §9 measures exactly how much it costs for keys
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
be serialised. ⬜ When persistence arrives in Stage 10, snapshots will have to
convert each deadline into a *remaining duration* at save time (or into
wall-clock), and rebuild deadlines on load. That is the right trade — correctness
while running matters more than convenience while saving — but it is a real
consequence, not a free lunch.

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

1. **The cache is not thread-safe yet.** A background thread mutating the map and
   the list while a caller is holding an iterator into them is a data race and an
   almost-guaranteed use-after-free. Locking is Stage 8. Adding a thread *now*
   would mean inventing the concurrency design here, badly, as a side effect of a
   feature about time — and the resulting bug would look like a TTL bug.

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

⬜ The plan: **Stage 8 first** (make it thread-safe), then active expiry on top,
with the sampling rate chosen from measurements of how much expired memory
actually accumulates under the benchmark workloads.

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

## 7. Complexity

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
the key twice (§8), the value once, two list pointers, and the map's node and
bucket overhead. For a 16-byte key and a 64-byte value that is an estimated
~180–200 bytes for ~80 bytes of payload. ⬜ *That figure is an estimate from the
data layout, not a measurement; measuring it properly is Stage 11 work.*

A caveat that matters more than the table: **every operation here is O(1), and
they still differ by roughly 10× in measured cost** (§9). The constant factors —
allocation, copying, and memory locality — dominate at this scale. The table is
the right answer to "how does this scale?" and the wrong answer to "which is
fastest?".

---

## 8. Ownership and Lifetime

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
different trade-off once Stage 8 makes the cache multi-threaded.

### No hidden insertion

`get` and `contains` use `find`, never `operator[]`. `operator[]` on an
`unordered_map` **default-constructs a value when the key is absent**, so a
lookup miss would silently insert an entry — and in a bounded cache that means a
stream of misses would evict real data to make room for empty placeholders. Two
tests pin this (`get_missing_key_returns_nullopt`,
`contains_does_not_insert_on_a_miss`).

---

## 9. Benchmark Methodology

Source: `benchmarks/cache_benchmark.cpp`. Results: `benchmarks/RESULTS.md`.

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DCACHEX_BUILD_BENCHMARKS=ON
cmake --build build/release -j
./build/release/bin/cachex_bench
```

### What is measured

| § | Section | Purpose |
| --- | --- | --- |
| 1 | Core operations | Six phases (`SET insert/update`, `GET hit/miss`, `CONTAINS`, `ERASE`) on an unbounded cache — continuity with the Stage 2 baseline |
| 2 | Cost of LRU | The same request sequence against an unbounded cache, a bounded cache with headroom, and a bounded cache that evicts |
| 3 | Workload A | 90% GET / 10% SET, skewed keys, hit-dominated |
| 4 | Workload B | 20% GET / 80% SET, uniform keys, eviction-dominated |
| 5 | Hit rate vs capacity | The point of LRU, as a curve |
| 6 | Cost of TTL checks | GET hits with no TTL, with a TTL, and over expired entries |

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
(batching identical operations, or hardware counters) — ⬜ Stage 5.

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

- **Memory footprint per entry.** ⬜ Stage 11. The figure in §6 is an estimate from the layout, not a measurement.
- **Concurrency scaling.** ⬜ Stage 8.
- **TTL expiry cost.** ⬜ Stage 4.
- **Latency below the clock tick.** ⬜ Stage 5, as above.

### Observations on the TTL measurement (§6 of the harness)

This section is the one place where the same paired method produced a **clearly
resolvable** answer rather than "below the noise floor", which is worth dwelling
on because it is what makes the method credible.

- **The TTL check costs ~12% of a GET hit** (median +11.7% to +12.7% across five runs, middle half consistently +10% to +14%). In absolute terms ~12 ns on a ~104 ns operation — which is about what one `steady_clock::now()` read costs, and the clock read is exactly what the check adds.
- **Compare that to §2**, where the LRU capacity check straddled zero under the identical method. Two effects, one method, two different verdicts: that is evidence the harness can tell signal from noise rather than always shrugging.
- **The `optional` short-circuit is doing real work.** Keys *without* a TTL pay none of that 12%, because `has_value()` is tested before the clock is read. The cost is paid only by the keys that asked for it.
- **The latency percentiles cannot see this effect at all** — p50 reads 208 ns for both cases, because the 41 ns clock tick is three times larger than the 12 ns difference. The throughput measurement resolves what the percentile table cannot, which is a good illustration of why both are reported.
- **Walking a cache of entirely expired entries** runs at ~6.3 M ops/sec vs ~9.6 M for live hits. That path is not simply slower: those calls return nothing *and* delete an entry, so it is a different operation, not a penalty on the same one.

### Latest results

Full numbers, hardware, and interpretation: **`benchmarks/RESULTS.md`**.

Headline findings from the Stage 3 run:

- **Hit rate tracks capacity as LRU predicts** on skewed traffic: 3.3% at 1% capacity, 30.9% at 10%, 84.2% at 40%, 100% at full capacity. This is the clearest evidence in the project that eviction is choosing the *right* victims — a random-eviction policy would not produce this curve.
- **The cost of the capacity check is below the harness's noise floor.** The paired comparison straddles zero. That is the expected result for one predictable branch per insert, and it is explicitly *not* a claim that LRU is free — it means this experiment cannot resolve a cost that small.
- **Eviction-heavy churn is ~1.4× more expensive per request than hit-heavy traffic** (217 ns vs 152 ns). Every miss in Workload B costs a failed lookup, an insert, *and* an eviction.
- `GET hit` still costs ~6× `CONTAINS`, and the gap is still the splice plus the value copy — unchanged by Stage 3, as it should be.

---

## 10. Current Components

| Path | Purpose | Status |
| --- | --- | --- |
| `CMakeLists.txt` | Language standard, build options, warning flags | ✅ |
| `include/cachex/cache.hpp` | Public `Cache` API, capacity and eviction | ✅ |
| `include/cachex/recency_list.hpp` | `Entry` and `RecencyList` | ✅ |
| `src/cache.cpp`, `src/recency_list.cpp` | Implementations | ✅ |
| `src/main.cpp` | Short in-process demo; ⬜ becomes the server in Stage 6 | ✅ |
| `tests/cache_test.cpp` | Cache semantics and edge cases | ✅ |
| `tests/lru_test.cpp` | Capacity, eviction, and recency ordering | ✅ |
| `tests/ttl_test.cpp` | Expiry, TTL query, and lazy-reclamation behaviour | ✅ |
| `tests/recency_list_test.cpp` | Ordering and iterator stability | ✅ |
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

## 11. Design Decisions

### 11.1 Why C++17 rather than C++20

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
| Heterogeneous lookup with `string_view` | ⚠️ The one real loss. `unordered_map<string, T>::find(string_view)` needs C++20's transparent hashing; in C++17 a lookup from a `string_view` must construct a temporary `std::string`. It does not bite yet, but it will when the protocol parser hands over views into a socket buffer (Stage 7). The fix is a transparent hash functor, not a language upgrade. |

### 11.2 Why CMake

The de-facto standard for C++, so it is what reviewers expect and what IDEs,
`clangd`, sanitizers, and CI already understand. It makes Debug/Release a
configuration flag rather than hand-maintained compiler invocations. The CMake
here is **target-based**: properties attach to targets rather than directory-wide
globals, so include paths and flags travel with the target that needs them.

### 11.3 Why warnings are an INTERFACE target

`cachex_warnings` carries no code, only flags; targets opt in by linking it.
Appending to global `CMAKE_CXX_FLAGS` would apply them to any third-party library
added later, whose warnings are not ours to fix — and hundreds of them scrolling
past is how a real warning in our own code gets missed.

`-Werror` is available (`-DCACHEX_WARNINGS_AS_ERRORS=ON`) but off by default, so a
new compiler version with a new warning cannot break someone's clone.

### 11.4 Why the test framework is hand-written

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

### 11.5 Why the tests run under sanitizers

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

### 11.6 Why the version header is generated

`configure_file()` expands `include/cachex/version.hpp.in` into the build
directory, substituting the version from `project()`. Otherwise the version lives
in two places and drifts. The generated header goes in the build tree, never the
source tree.

### 11.7 Why `build/` is not committed

Build output is reproducible from the sources, specific to one compiler and one
machine, goes stale the instant a flag changes, and makes every diff unreadable.
Commit the inputs, never the outputs.

---

## 12. Interview Questions I Should Be Able To Answer

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

- **Memory overhead is high** — an estimated ~180–200 bytes per entry for ~80 bytes of payload. Two node allocations per entry (list node and map node), plus the key stored twice, plus bucket overhead. Redis avoids much of this with *approximated* LRU: it samples a handful of random keys and evicts the oldest of the sample, which needs no list at all.
- **Poor cache locality.** Both containers are node-based, so a `get` chases pointers through scattered memory. The benchmark shows this clearly: `SET update` costs ~3.5× `SET insert` despite doing strictly less work, purely because of access patterns.
- **Every read is a write.** A `get` mutates the list. That is free today and becomes a real problem in Stage 8: readers cannot share a read-lock if they all need to reorder, which is exactly why LRU is hard to make concurrent and why sharding (Stage 9) matters.
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
meaningless outside the process and cannot be serialised. Stage 10 will have to
convert deadlines to remaining durations when snapshotting. §6.2.

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

Order of reasons: (1) the cache is not thread-safe yet, so a sweeper mutating the
map while a caller holds an iterator is a data race and a use-after-free — and the
bug would present as a TTL bug. (2) The right sampling policy needs measurements
that do not exist yet; Redis's "20 keys, repeat while >25% expired" are tuned
constants, not first principles. (3) Sampling a random key from an
`unordered_map` is not O(1), so active expiry needs a data structure the cache
does not currently have. Thread safety first (Stage 8), then active expiry. §6.5.

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
free for keys that do not use it. §9.

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

### How would you test that eviction is correct?

Not by checking `size()` — that passes even if the *wrong* entry is evicted. The
tests assert the full recency order as a string (`"d,c,b"`), so a failure shows
the whole state rather than just which key went missing. They cover the boundary
capacities (0, 1, exactly full), each operation's effect on recency, and a churn
test that verifies the map and the list still agree on which keys exist.

---

## 13. Future Roadmap

**Everything below is ⬜ future work.**

| # | Stage | What it adds | Core concepts | Status |
| --- | --- | --- | --- | --- |
| 1 | Project foundation | CMake, structure, warnings, test harness | Build systems, testability | ✅ |
| 2 | Core cache | `set`/`get`/`erase`/`contains`/`size`, hash map + list | Hash tables, iterator invalidation, ownership | ✅ |
| 3 | **LRU eviction** | Capacity limit, O(1) eviction, workload benchmarks | Why O(1) LRU needs both structures; hit rate | ✅ |
| 4 | **TTL** | Per-key expiry deadlines, lazy expiration | Lazy vs. active expiry; `steady_clock` vs. `system_clock` | ✅ |
| 5 | Benchmark harness II | Sub-tick latency, better measurement environment | Why the average latency lies | ⬜ |
| 8+ | Active expiry | Background sweep, once locking exists | Sampling policies; why it cannot come before thread safety | ⬜ |
| 6 | TCP server | `socket`/`bind`/`listen`/`accept`, one client | The socket API; blocking I/O; partial reads | ⬜ |
| 7 | Client protocol | Line-based text protocol and parser | Framing, buffering, malformed input | ⬜ |
| 8 | Concurrency | Many clients — thread pool or event loop | Data races, why `shared_mutex` disappoints for LRU | ⬜ |
| 9 | Sharded cache | N independently locked shards | Lock contention as the real bottleneck | ⬜ |
| 10 | Persistence | Snapshot to disk, restore on startup | Serialisation; durability vs. throughput | ⬜ |
| 11 | Final optimisation | Profile, tune, re-benchmark against the Stage 2/3 baselines | Cache locality, allocation cost, proving an improvement | ⬜ |

Each stage ends with this document updated: components in §10, decisions in §11,
new questions in §12, and the diagram in §3 grown to match what actually exists.
