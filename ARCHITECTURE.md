# CacheX Architecture

CacheX is an in-memory key-value cache server written in C++17. This document
describes the whole system, from the data structures up to the network server,
and reads top to bottom. Measured numbers appear only in §14; every one of them
comes from the raw benchmark output committed in `benchmarks/results/`.

**Contents**

1. [Problem statement](#1-problem-statement)
2. [Requirements](#2-requirements)
3. [Non-goals](#3-non-goals)
4. [High-level architecture](#4-high-level-architecture)
5. [Cache data structures](#5-cache-data-structures)
6. [LRU](#6-lru)
7. [TTL](#7-ttl)
8. [Networking protocol](#8-networking-protocol)
9. [TCP server](#9-tcp-server)
10. [Concurrency model](#10-concurrency-model)
11. [Sharding](#11-sharding)
12. [Persistence](#12-persistence)
13. [Benchmark methodology](#13-benchmark-methodology)
14. [Performance results](#14-performance-results)
15. [Tradeoffs](#15-tradeoffs)
16. [Failure modes](#16-failure-modes)
17. [Future improvements](#17-future-improvements)
- [Appendix: interview questions](#appendix-interview-questions)

---

## 1. Problem statement

Reading from a database costs milliseconds; reading from memory costs
nanoseconds. Real traffic is skewed — a small fraction of keys serves most
requests — so keeping that hot subset in memory in front of the slow store
answers most requests without touching it.

Building that cache means answering four questions:

- **Memory is finite.** What gets evicted when the cache is full, and can the
  choice be made in constant time?
- **Data goes stale.** How does an entry expire, and what does expiry cost?
- **Clients live in other processes.** How are requests framed and answered
  over a byte stream?
- **Many clients arrive at once.** How do concurrent readers and writers share
  one data structure without corrupting it or serialising everything?


---

## 2. Requirements

### Functional

| Requirement | Implementation |
| --- | --- |
| `SET`, `GET`, `DELETE`, `EXISTS` on string keys and values | `Cache` (§5) |
| Bounded memory with least-recently-used eviction | O(1) LRU (§6) |
| Per-key time-to-live, and a query for the time remaining | Lazy expiration (§7) |
| Network access with a documented protocol | Line-based text protocol over TCP (§8, §9) |
| Many simultaneous clients | Thread per connection (§10) |
| Throughput that does not collapse under contention | Sharded cache (§11) |
| Survive a planned restart | Snapshots, `SAVE` / `LOAD` (§12) |

### Non-functional

| Requirement | How it is met |
| --- | --- |
| O(1) average `GET`, `SET`, `DELETE` and eviction | Hash map plus doubly linked list (§5) |
| No undefined behaviour or data races | 221 tests, run clean under AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer |
| Malformed input never crashes the server | Strict parsing, input limits, validated snapshot sizes (§8, §12) |
| No external dependencies | Standard library and POSIX only; a ~100-line test framework |
| Reproducible performance claims | Fixed seeds, committed raw benchmark output (§13) |

---

## 3. Non-goals

Each of these is excluded deliberately.

- **Clustering, replication, failover.** Distributed consensus is a separate problem; a single node already exercises the data structures and concurrency.
- **Durability.** Snapshots survive a planned restart, not a crash or power loss (§12). CacheX is a cache, not a system of record.
- **Rich data types.** No lists, sets, hashes or sorted sets. The interesting problems are eviction, expiry and concurrency.
- **Authentication and TLS.** CacheX assumes a trusted network, as Redis does by default.
- **Transactions and compound atomic operations.** Each command is atomic; sequences of commands are not (§10).
- **Lock-free data structures.** Sharded locking delivers the scalability needed at a small fraction of the risk.
- **Approximate LRU.** Redis samples keys to save memory. CacheX keeps exact LRU so the O(1) structure is visible.

---

## 4. High-level architecture

```
   cachex_client / netcat / any TCP client
                  |
                  |  TCP, one command per line (§8)
                  v
   +------------------------------------------+
   |  Server        accept loop               |  server.cpp
   +------------------------------------------+
        |  one worker thread per connection
        v
   +------------------------------------------+
   |  Connection    recv loop, LineBuffer     |  connection.cpp, line_buffer.cpp
   |  parse_command -> execute -> reply_*     |  protocol.cpp, command_handler.cpp
   +------------------------------------------+
        |                                  |
        |  GET SET DELETE EXISTS TTL       |  SAVE / LOAD
        v                                  v
===========================================|==== nothing below knows about sockets
   +--------------------------------+   +----------------------------+
   |  ShardedCache                  |<--|  PersistenceManager        |
   |  shard = mix(hash(key)) % N    |   |  export_entries() -> file  |
   +--------------------------------+   |  file -> set()             |
        |        |    ...    |          |  PeriodicSaver (optional)  |
        v        v           v          +----------------------------+
   +---------+ +---------+ +---------+               |
   |SyncCache| |SyncCache| |SyncCache|               v
   | mutex   | | mutex   | | mutex   |      snapshot file
   | Cache   | | Cache   | | Cache   |      (temp file + atomic rename)
   +---------+ +---------+ +---------+
        |
        v   inside each Cache
   +----------------------+   +------------------------------+
   |     RecencyList      |<--|            index_            |
   |   std::list<Entry>   |   |  unordered_map<string,       |
   |  head (MRU) ... tail |   |    RecencyList::Iterator>    |
   +----------------------+   +------------------------------+
     eviction at the tail       O(1) lookup
```

### Components

| Component | Responsibility | Files |
| --- | --- | --- |
| `RecencyList`, `Entry` | Owns entries in most- to least-recently-used order | `recency_list.hpp/.cpp` |
| `Cache` | Single-threaded engine: lookup, LRU, TTL | `cache.hpp/.cpp` |
| `SyncCache` | One mutex around a `Cache`; one shard | `sync_cache.hpp` |
| `ShardedCache` | N `SyncCache` shards selected by key hash | `sharded_cache.hpp/.cpp` |
| `PersistenceManager`, `PeriodicSaver` | Snapshot save and load | `persistence.hpp/.cpp` |
| `LineBuffer` | Turns a byte stream into lines | `line_buffer.hpp`, `net/line_buffer.cpp` |
| `parse_command`, `reply_*` | Protocol grammar and reply formatting | `protocol.hpp`, `net/protocol.cpp` |
| `execute` | Applies a parsed command to the cache | `command_handler.hpp`, `net/command_handler.cpp` |
| `Socket`, `send_all`, `connect_to`, `parse_port` | RAII file descriptors and socket helpers | `socket.hpp`, `net/socket.cpp` |
| `Connection` | Serves one client until it disconnects | `connection.hpp`, `net/connection.cpp` |
| `Server` | Listening socket, accept loop, worker threads | `server.hpp`, `net/server.cpp` |

### Libraries and the dependency rule

```
cachex_core   cache · recency_list · sharded_cache · persistence · version
    |         (sync_cache is header-only)          knows nothing about sockets
    |
    +--> cachex_net   line_buffer · protocol · socket · command_handler
    |        |        connection · server
    |        +--> cachex_server, cachex_client, cachex_tests, cachex_net_bench
    |
    +--> cachex (demo), cachex_bench, cachex_persist_bench, cachex_bench_suite
```

`cachex_core` does not link `cachex_net`, so cache code cannot depend on socket
code — it would fail to link. The dependency runs one way: the network layer
calls the cache. This is what makes `execute(ShardedCache&, Command,
PersistenceManager*)` testable with no socket at all.

### Life of a request

```
  "GET user:1\n" arrives on a client socket
     -> Connection::fill_buffer()      recv() appends bytes to the LineBuffer
     -> LineBuffer::next_line()        "GET user:1"
     -> parse_command()                Command{Get, key="user:1"}
     -> execute()                      ShardedCache::get("user:1")
          -> shard_index_for()           choose one shard
          -> SyncCache::get()            lock that shard only
               -> Cache::get()             hash lookup, splice to head, copy value
     -> reply_value()                  "=Tanishq\n"
     -> send_all()                     loops until every byte is written
```

---

## 5. Cache data structures

### Types

```cpp
struct Entry {
  std::string key;
  std::string value;
  std::optional<std::chrono::steady_clock::time_point> expires_at;  // nullopt: never
};

class RecencyList {                       // owns every Entry, MRU at the front
  std::list<Entry> entries_;
};

class Cache {
  RecencyList entries_;                                             // owner
  std::unordered_map<std::string, RecencyList::Iterator> index_;    // borrower
  std::optional<std::size_t> capacity_;                             // nullopt: unbounded
  std::size_t evictions_, expired_removals_;
};
```

```
  index_ (hash map)                    entries_ (doubly linked list)
  -----------------                    -----------------------------
  "user:2"  --------------------+
  "user:1"  ------------+       |
  "user:9"  ---+        |       |
               v        v       v
  head  [user:9] <-> [user:1] <-> [user:2]  tail
        (newest)                  (oldest: next to evict)
```

### Why a hash map

The defining operation is "given a key, find its value", which a hash table does
in O(1) average time regardless of how many keys are stored. A `std::map` would
be O(log n) and would order keys for no benefit. The standard container is used
rather than a custom table because the design point is the combination of
structures, and its semantics are universally known.

Its costs: O(1) is average, not worst case (colliding keys degrade to O(n));
every element is a separate allocation; growth rehashes all elements.

### Why a doubly linked list

The list answers what the map cannot: which entry has gone longest unused.
`std::list` provides the three properties LRU needs:

1. **O(1) splice.** Moving a node to the front rewrites a few pointers; nothing is copied or allocated.
2. **Stable iterators.** Inserting or erasing one element leaves iterators to all others valid, so the map can hold an iterator per key indefinitely. A `std::vector` would invalidate every stored iterator on reallocation.
3. **O(1) access to both ends.** The tail is always the eviction candidate.

It must be doubly linked because unlinking a node needs its predecessor; a
singly linked list would have to walk from the head, which is O(n).

### Ownership and lifetime

- The list owns every `Entry`. The map holds non-owning iterators. There is no `new`, `delete` or smart pointer in the cache.
- Members are declared owner first, so the map (borrower) is destroyed before the list (owner).
- **Invariant:** every iterator in `index_` points at a live node, and every node is referenced by exactly one map entry. `size()` of the two structures always matches, which the tests check under concurrency.
- Removal order matters: `erase` unlinks the list node through the iterator stored in the map, then erases the map entry. Reversing the two reads a destroyed iterator.

### The key is stored twice

`Entry::key` duplicates the map key. Eviction runs list to map: the list knows
the oldest node, but removing it from the map requires its key. Without the copy
in the node, finding the map entry would mean scanning the map. The duplicate
costs one string per entry and buys O(1) eviction.

### Reads return a copy

`get()` returns `std::optional<std::string>`:

- **optional** because an empty stored value must be distinguishable from a missing key.
- **a copy** because a reference would dangle as soon as another call evicted or erased the entry — and on a full cache any `set()` can do that.

`get_into(key, buffer)` has identical semantics but assigns into a caller-owned
buffer, reusing its capacity. For values beyond the small-string limit this
removes one allocation and one free per hit (§14).

### Complexity

| Operation | Average | Worst case | Worst case comes from |
| --- | --- | --- | --- |
| `set` | O(1) | O(n) | hash collisions, or a rehash on insert |
| `get`, `get_into` | O(1) | O(n) | hash collisions (the splice is always O(1)) |
| `erase`, `contains`, `ttl` | O(1) | O(n) | hash collisions |
| eviction | O(1) | O(1) amortised | tail lookup is O(1); map erase is average O(1) |
| `size`, `capacity`, counters | O(1) | O(1) | |
| `clear`, `export_entries` | O(n) | O(n) | every entry is touched |

Space is O(n), bounded by capacity. The measured cost per entry is in §14.

---

## 6. LRU

### Why the pair gives O(1)

| Need | Hash map alone | List alone | Both |
| --- | --- | --- | --- |
| Find a key | O(1) | O(n) | O(1) |
| Know the oldest entry | not possible | O(1) | O(1) |
| Mark an entry as used | nothing to update | O(1) only with a handle | O(1) |

The map's values are list iterators, so the list is never searched. Every
operation is a map lookup, a constant-time list operation, and a map update.

### What counts as a use

| Operation | Updates recency | Reason |
| --- | --- | --- |
| `get` / `get_into` hit | yes | the definition of LRU |
| `get` miss | no | there is nothing to reorder, and nothing is inserted |
| `set` (insert or update) | yes | a write is at least as strong a signal as a read |
| `contains` | no | peeking must not keep otherwise unused data alive |
| `ttl` | no | a metadata query must not rescue a key from eviction |
| `erase` | — | the entry is gone and its slot is free |

### GET hit

```
  get("b")          head [d] <-> [c] <-> [b] <-> [a] tail

  1. index_.find("b")         O(1) average   -> iterator to [b]
  2. check expiry             O(1)           -> live
  3. splice [b] to head       O(1)
  4. copy the value out

  result:           head [b] <-> [d] <-> [c] <-> [a] tail
```

### Eviction at capacity

```
  capacity 4:       head [d] <-> [c] <-> [b] <-> [a] tail

  set("e", v)
  1. index_.find("e")         miss, so this is an insert
  2. push [e] at head; index_.emplace("e", it)      size 5
  3. size > capacity:
       a. victim = entries_.oldest()                [a]
       b. index_.erase(victim.key)                  needs the key stored in the node
       c. entries_.pop_oldest()                     destroys [a]

  result:           head [e] <-> [d] <-> [c] <-> [b] tail
```

Insert happens before the trim, so the new entry is at the head and can never be
its own victim. With capacity 0 the same code inserts and immediately removes the
entry, so no special case exists. The trim is a loop, which in steady state runs
at most once.

Step 3b reads the victim's key by reference from the list node, erases the map
entry, and only then destroys the node. Destroying the node first would leave
that reference dangling.

---

## 7. TTL

### Model

Each `Entry` carries `std::optional<steady_clock::time_point> expires_at`:

- **An absolute deadline**, not a remaining duration, so "is this expired?" is one comparison against the current time.
- **optional** so "never expires" is a distinct state, and so an entry without a TTL never reads the clock: `has_value()` is checked first. Keys that do not use TTL pay nothing for it.

```cpp
bool is_expired(const Entry& e) {
  return e.expires_at.has_value() && *e.expires_at <= steady_clock::now();
}
```

### Why a monotonic clock

`system_clock` follows wall time and jumps: NTP corrections, manual changes, VM
resumes. A backwards jump keeps expired keys alive; a forwards jump expires the
whole cache at once and sends the next burst of traffic to the database.
`steady_clock` never jumps and measures elapsed time, which is what a TTL is.

The consequence: a `steady_clock` time point is meaningless in another process,
so snapshots store remaining durations rather than deadlines (§12).

### Lazy expiration

An expired entry is removed when an operation encounters it. The lookup has
already found the entry, so removal costs the normal O(1) teardown.

| Operation on an expired entry | Reports | Reclaims |
| --- | --- | --- |
| `get`, `get_into` | missing | yes |
| `ttl` | `Missing` | yes |
| `erase` | `false` (nothing visible was removed) | yes |
| `contains` | `false` | no — it is `const` and never mutates |

### Defined behaviour

| Case | Behaviour |
| --- | --- |
| `set(k, v)` on a key with a TTL | the TTL is cleared; a set replaces the whole entry |
| `set(k, v, ttl)` on an existing key | the deadline is replaced |
| `set(k, v, ttl)` with `ttl <= 0` | the key is erased and nothing is stored; after the call the key is visible if and only if `ttl > 0` |
| `ttl(k)` | `Missing`, `Persistent`, or `Expiring` with milliseconds remaining |
| `size()` | counts entries resident, including expired entries not yet reclaimed |

`ttl()` returns a tagged `TtlInfo` rather than an integer with sentinel values,
so "missing" can never be mistaken for a duration.

### Limits of lazy expiration

- An expired key that is never touched again is never reclaimed and keeps its memory.
- Expired entries still occupy capacity, so on a full cache LRU can evict a live entry while a dead one remains.
- `size()` is an upper bound on visible keys; an exact count would require a scan.

A background sweep would address the first two. It needs a policy for how much
to scan per cycle and a cheap way to sample keys, neither of which an
`unordered_map` provides (§17).

### How this differs from Redis

| | CacheX | Redis |
| --- | --- | --- |
| Expiry | lazy only | lazy plus an active sampling cycle |
| Clock | monotonic | wall clock |
| `TTL` reply | typed: `_`, `+NOEXPIRE`, `:seconds` | integer: `-2`, `-1`, seconds |
| `SET` with TTL 0 | deletes the key | error |
| Changing a TTL alone | not supported | `EXPIRE`, `PERSIST` |
| Eviction policy | exact LRU | configurable, approximate LRU |

---

## 8. Networking protocol

The full specification is `docs/PROTOCOL.md`. This section covers the design.

### Transport and application protocol

TCP provides reliable, ordered, de-duplicated delivery of a **byte stream**. It
has no message boundaries: one `recv()` can return half a command, one command,
or several. The application protocol exists to put those boundaries back and to
give the bytes meaning.

TCP rather than UDP because a cache client needs every request to arrive exactly
once and in order, and replies can exceed one datagram. Rebuilding that on UDP
would reproduce TCP.

### Framing and grammar

- One request per line, terminated by `\n`; a trailing `\r` is accepted.
- Tokens are separated by spaces or tabs; command names are case-insensitive.
- Every reply is one line whose first byte identifies its type.

| Request | Reply |
| --- | --- |
| `SET key value [ttl_seconds]` | `+OK` |
| `GET key` | `=value`, or `_` if absent |
| `DELETE key` (alias `DEL`) | `:1` or `:0` |
| `EXISTS key` | `:1` or `:0` |
| `TTL key` | `:seconds` (rounded up), `+NOEXPIRE`, or `_` |
| `SAVE`, `LOAD` | `:count` |
| `PING` | `+PONG` |
| `QUIT` | `+BYE`, then the connection closes |
| invalid input | `-ERR reason`; the connection stays open |

### Limits

| Limit | Value | Why |
| --- | --- | --- |
| Request line | 64 KiB | without it a client that never sends `\n` makes the server buffer without bound |
| TTL | 0 to 315,360,000 seconds | keeps deadline arithmetic from overflowing |
| Echoed token in an error | 32 characters | a large junk token is not reflected back |

`SAVE` and `LOAD` take no arguments. The snapshot path is server configuration;
accepting a path from the network would let any client read or overwrite files
the server can reach.

### Known limitation

Keys and values cannot contain whitespace, because tokens are whitespace
delimited. The cache stores arbitrary bytes; only the text protocol is limited.
A length-prefixed format removes this, and the snapshot format already uses one
(§12).

---

## 9. TCP server

### Socket lifecycle

```
  SERVER                                         CLIENT

  socket()     create an endpoint
  setsockopt   SO_REUSEADDR
  bind()       claim 127.0.0.1:port
  listen()     mark passive; the kernel now completes
               handshakes and queues them (backlog 128)
     |                                           socket()
     |   <======= SYN / SYN-ACK / ACK ========   connect()
     |   (kernel completes this with no application code running)
  poll()       wait up to 100 ms for a queued connection
  accept()     returns a NEW socket for this client;
               the listening socket keeps listening
     |   <======= "GET user:1\n" =============   send()
  recv()       whatever bytes have arrived
  send_all()   ======= "=Tanishq\n" =========>   recv()
     |   <======= FIN ========================   close()
  recv() == 0  end of stream; close this socket
```

- `listen()` does not block or accept anything; it changes the socket's state.
- A client's `connect()` succeeds as soon as the kernel finishes the handshake, before `accept()` runs.
- `accept()` returns a different socket from the listening one.

### Reading: partial and batched data

`Connection::serve()` never assumes one `recv()` is one request:

```cpp
while (true) {
  while (auto line = buffer_.next_line()) {   // drain every complete line first
    if (!handle_line(*line)) return;
  }
  if (buffer_.buffered() > kMaxLineBytes) { /* reply -ERR, close */ }
  if (!fill_buffer()) return;                  // recv() == 0: client closed
}
```

Draining all buffered lines before reading again is what makes pipelined
clients work. `LineBuffer` contains no socket code, so split and batched input
is tested by choosing exactly where to cut the bytes.

### Writing: partial sends

`send()` may accept fewer bytes than offered when the kernel's send buffer is
full. `send_all()` loops until every byte is written; ignoring the return value
would truncate replies and corrupt the stream.

### Parsing and dispatch

- `parse_command(std::string_view)` returns a `ParseResult` rather than throwing, because malformed input is expected on a public socket.
- Integers are parsed with `std::from_chars`, which rejects `12abc` instead of reading `12`.
- `execute()` maps a `Command` to cache calls and returns the reply string. It knows nothing about sockets.

### Socket details

| Detail | Reason |
| --- | --- |
| `Socket` is a move-only RAII owner | a copied descriptor would be closed twice, and the second close could close an unrelated file that reused the number |
| `TCP_NODELAY` | Nagle's algorithm plus delayed ACKs add tens of milliseconds to small request/response exchanges |
| `MSG_NOSIGNAL` / `SO_NOSIGPIPE` | writing to a disconnected peer raises `SIGPIPE`, whose default action kills the process |
| retry on `EINTR` | a signal interrupting a system call is not an error |
| `SO_REUSEADDR` | lets the server restart while old connections sit in `TIME_WAIT` |
| port `0` | the kernel picks a free port; the tests use this to avoid collisions |

### Connection lifecycle

A connection carries any number of commands and ends when the client sends
`QUIT` (acknowledged with `+BYE` before closing), when the client disconnects,
or when a line exceeds the limit. Command errors do not end a connection.

### Startup and shutdown

`cachex_server [port] [capacity] [shards] [snapshot] [save-secs]` parses every
argument strictly, loads the snapshot if one is configured, then starts
listening. `SIGINT` and `SIGTERM` set an atomic stop flag. The accept loop sees
it within one 100 ms poll, stops accepting, and joins every worker. The periodic
saver is then stopped and a final snapshot written.

---

## 10. Concurrency model

### Thread per connection

```
  accept loop (1 thread)
       |
       +--> worker 1 --> Connection --+
       +--> worker 2 --> Connection --+--> ShardedCache (shared)
       +--> worker N --> Connection --+
```

Every accepted connection gets its own worker thread. Everything a worker uses —
its socket, buffer and parsed commands — is private to it, except the cache.
Connections beyond `max_connections` (default 256) receive
`-ERR server at connection limit (256)` and are closed, which bounds the number
of threads.

### Shared state and locks

| State | Protected by | Held for |
| --- | --- | --- |
| Each shard's `Cache` | that shard's `std::mutex` (inside `SyncCache`) | one cache operation |
| The server's list of worker threads | `Server::workers_mutex_` | adding or reaping entries; never while a join blocks |
| Stop flag, connection counters, saver counters | `std::atomic` | single reads and writes |

`SyncCache` wraps a `Cache` and takes the lock in every method with
`std::lock_guard`, so callers cannot forget to lock and the lock is released on
every path out. Each critical section is exactly one cache operation, and **no
socket or file I/O happens while a lock is held**, so a slow client or disk
never blocks other clients.

**No deadlock is possible**: no code path holds two of these mutexes at once.

### GET takes an exclusive lock

Under LRU a read is a write. `get()` splices the entry to the head of the list
and may remove an expired entry. Two concurrent "reads" are two concurrent list
mutations. `std::shared_mutex` would not help: the common operation in a cache is
exactly the read that must be exclusive.

Without the lock the structure corrupts. Running the unsynchronised `Cache` from
four threads under ThreadSanitizer reports data races in the map rehash and list
splice, and the process hangs on a cycle in the corrupted list. The same workload
through `SyncCache` or `ShardedCache` reports nothing.

### What is and is not atomic

Every individual command is atomic. Sequences are not:

```cpp
int n = std::stoi(cache.get("counter").value_or("0"));   // another thread
cache.set("counter", std::to_string(n + 1));             // can run between these
```

Two clients doing this can both read the same value and lose an update. A test
asserts this behaviour. Fixing it requires a compound operation executed under
one lock (for example `INCR`), not a larger lock.

### Worker lifecycle

```
  accept -> spawn_worker: create thread; push {thread, finished flag}
               worker: serve connection; decrement active count; set finished
  every accept-loop iteration: join and remove workers whose flag is set
  stop: the loop exits; join every remaining worker
```

- Workers are joined, never detached. A detached worker could outlive the server and the cache it references.
- `std::thread` cannot report whether it has finished, so each worker sets a shared atomic flag as its last action.
- If the operating system refuses to create a thread, the error is caught, the connection is closed, and the server keeps running.

### Where coarse locking stops scaling

With one mutex the whole cache is a single sequential section: extra threads
queue on it and add handoff cost. §11 splits it into independent sections, and
§14 shows the effect.

---

## 11. Sharding

### Design

```
                          ShardedCache
                               |
        +--------------+-------+-------+--------------+
        |              |               |              |
     Shard 0        Shard 1         Shard 2       Shard N-1
    SyncCache      SyncCache       SyncCache      SyncCache
    mutex+map      mutex+map       mutex+map      mutex+map
    +LRU+cap       +LRU+cap        +LRU+cap       +LRU+cap
```

Each shard is a complete `SyncCache` with its own mutex, hash map, recency list
and capacity. Each operation locks exactly one shard, so threads working on keys
in different shards never wait for each other. The default is 8 shards.
`ShardedCache` has the same interface as `SyncCache`, so the network layer is
unaware of it.

### Shard selection

```cpp
uint64_t h = std::hash<std::string>{}(key);
h ^= h >> 30;  h *= 0xbf58476d1ce4e5b9ULL;      // splitmix64 finaliser
h ^= h >> 27;  h *= 0x94d049bb133111ebULL;
h ^= h >> 31;
shard = h % shard_count;
```

Each shard's `unordered_map` also hashes the key with `std::hash` to choose a
bucket. Using the raw hash for the shard as well would correlate shard and bucket
choice, clustering keys into few buckets per shard. The mixing step removes the
correlation. A test requires 8,000 keys to land within ±25% of even across 8
shards.

### Capacity distribution

Total capacity is divided evenly and the remainder goes to the first shards, so
the parts sum to exactly the requested total (10 across 4 shards is 3, 3, 2, 2).
If the total is smaller than the shard count, the shard count is reduced so that
no shard has capacity 0 and silently drops every key that maps to it.

### Correctness tradeoffs

- **LRU is per shard.** Each shard evicts its own oldest entry, so a hot key in a crowded shard can be evicted while a colder key in a quiet shard survives. With evenly distributed keys the effect on hit rate is negligible (§14).
- **Aggregates are not snapshots.** `size()`, `evictions()` and `expired_removals()` lock shards one at a time. They are exact when the cache is idle and approximate under load; an exact value would require locking every shard at once, which is the global lock again.
- **Single-key behaviour is unchanged.** Results of `get`, `set`, `erase`, `contains` and `ttl` do not depend on the shard count; a test checks this at 1, 4 and 16 shards.

### When sharding helps

It helps when threads contend for the cache and keys spread across shards. It
does not help when there is no contention, when another component such as the
network is the bottleneck, or when load is concentrated on a single key. §14
measures each case.

---

## 12. Persistence

### Architecture

```
   ShardedCache --export_entries()--> PersistenceManager --> snapshot file
        ^                                   |
        +------------ set() ----------------+
```

`PersistenceManager` uses only the cache's public interface: `export_entries()`
to read and ordinary `set()` calls to restore. It knows nothing about sockets.
Persistence is enabled by giving the server a snapshot path; without one,
`SAVE` and `LOAD` return `-ERR persistence is not enabled on this server`.

`export_entries()` returns plain values:

```cpp
struct EntrySnapshot {
  std::string key;
  std::string value;
  std::optional<std::chrono::milliseconds> remaining_ttl;   // nullopt: never expires
};
```

### Format

```
CACHEX-SNAPSHOT 1 <saved_at_unix_ms>\n
<entry_count>\n
<key_len> <value_len> <ttl_ms>\n          repeated entry_count times
<key bytes><value bytes>\n
```

- Lengths precede the payload and bytes are copied verbatim, so keys and values may contain spaces, newlines and NUL bytes.
- `ttl_ms` is `-1` for no expiry, otherwise the milliseconds remaining at save time.
- The trailing newline after each payload is redundant given the lengths, which is why it is checked: a mismatch detects a corrupt file.

### TTL across a restart

Deadlines cannot be stored, because `steady_clock` time points do not survive the
process (§7). Remaining durations alone are also wrong: a key saved with 60 ms
left would return with a fresh 60 ms however long the server was down. The
header therefore records wall-clock time at save. On load:

```
elapsed   = max(0, wall_now - saved_at)        clamped so a clock jump never extends a TTL
remaining = ttl_ms - elapsed                   if remaining <= 0, the entry is skipped
```

`system_clock` is used only for this measurement; running expiry uses
`steady_clock`.

### Snapshot lifecycle

```
  startup   load(): missing file is normal; corrupt file is logged and the
            server starts empty rather than refusing to run
  running   SAVE command (synchronous for the requesting client)
            PeriodicSaver thread, if an interval is configured
  shutdown  one final save
```

`PeriodicSaver` waits on a condition variable so it stops immediately, and it is
joined in its destructor.

### Consistency

**Every snapshot is well formed.** Each shard is locked, copied and unlocked, so
every record is complete. The file is written only after copying finishes, so no
lock is held during disk I/O.

**A snapshot is not a single point in time.** Shards are copied one after
another, so a write landing between two shard copies may or may not be included.
A global instant would require locking all shards at once.

**Writes are atomic.** Each save writes its own uniquely named temporary file
(process id plus a counter) and renames it over the snapshot. POSIX `rename` is
atomic, so readers see the old file or the new one, never a partial file.
Unique names make concurrent saves safe: two clients sending `SAVE`, or `SAVE`
racing the periodic saver, each produce a complete file and the last rename wins.

**Loads are all or nothing.** The whole file is parsed and validated before any
entry is applied, so a corrupt snapshot leaves the cache unchanged.

**Sizes read from disk are untrusted.** The entry count is checked against the
file size (the smallest record is 7 bytes) and each record's lengths against the
file size, before anything is allocated. A corrupt count or length is reported
as a bad file instead of reaching `reserve()` or `resize()` and throwing.
Lengths are bounded by the file size rather than by the bytes remaining, because
asking the stream for its position on every record makes a load about five
times slower.

Records are replayed in reverse so that recency order is restored within each
shard.

### What persistence does not provide

| | CacheX | A database |
| --- | --- | --- |
| Crash durability | writes since the last save are lost | write-ahead log |
| `fsync` | not called; a power loss can lose a completed save | forces data to disk before acknowledging |
| Point-in-time consistency | per shard | MVCC or copy-on-write snapshot |
| Incremental writes | full rewrite, O(n) | log of changes |
| Integrity | length checks only | checksums |
| Replication | none | yes |

Snapshots let a cache survive a planned restart. Data in CacheX must always be
recoverable from its source of truth.

---

## 13. Benchmark methodology

### Benchmarks

| Binary | Measures |
| --- | --- |
| `cachex_bench_suite` | 5 workloads × 3 cache versions × 1–16 threads; memory per entry; `get()` vs `get_into()` |
| `cachex_net_bench` | Over TCP: single-client `PING`/`SET`/`GET`; 1–16 clients × 1/2/4/8 shards; the same shard matrix in-process; hit rate of global vs per-shard LRU |
| `cachex_persist_bench` | Snapshot save time, load time and size for 1,000–500,000 entries |
| `cachex_bench` | Single-threaded `Cache` operations, the cost of the capacity check, two workloads, hit rate against capacity, the cost of the TTL check |

`benchmarks/run_all.sh` builds nothing; it runs all four against an existing
Release build, runs the suite five times, and writes raw output and an
`environment.txt` (machine, OS, compiler, git commit, uncommitted-changes flag,
load average) to `benchmarks/results/`. Those files are committed.

### Cache versions compared by the suite

| Version | Type | Locking |
| --- | --- | --- |
| A — baseline | `Cache` | none; single thread only |
| B — global mutex | `SyncCache` | one mutex |
| C — sharded | `ShardedCache(8)` | 8 mutexes |

### Workloads

400,000 operations each, 64-byte values, 16-byte keys. "Skewed" sends 80% of
operations to 20% of keys. Cache-aside means a `GET` miss writes the key, as an
application loading from its database would.

| Workload | Mix | Keys | Capacity | Notes |
| --- | --- | --- | --- | --- |
| `read-heavy` | 90% GET / 10% SET | 100,000 skewed | 40,000 | typical cache traffic |
| `balanced` | 50% / 50% | 100,000 skewed | 40,000 | |
| `write-heavy` | 10% / 90% | 100,000 skewed | 40,000 | |
| `high-churn` | 20% / 80% | 200,000 uniform | 20,000 | no hot set; constant eviction |
| `ttl-heavy` | 70% / 30% | 100,000 skewed | 40,000 | every SET carries a 2 s TTL |

### Rules

| Rule | Reason |
| --- | --- |
| Release build (`-O3 -DNDEBUG`); each binary warns if assertions are enabled | a Debug build measures the absence of the optimiser |
| Fixed seeds; operation sequences generated before timing | every version replays identical work; random number generation is never timed |
| All threads released together by a start gate | an N-thread run has N threads in flight for its whole duration |
| A discarded warm-up pass before measuring | page faults and allocator growth would otherwise be charged to whichever configuration runs first |
| Throughput from the wall time of the whole run | not limited by clock resolution |
| Median of five suite runs, with the range reported | one run on a shared machine proves little |
| Paired, order-alternated comparisons for small effects | machine drift and heap warm-up are larger than the effects being measured |
| A lock-free `PING` control over TCP | shows whether the transport, not the cache, limits scaling |
| Results written to a printed checksum | prevents the optimiser deleting work whose result is otherwise unused |
| Memory measured as malloc bytes in use | resident set size does not shrink when memory is freed, so its deltas are unreliable |

### Limits of the measurements

- **Latency percentiles are quantised to the clock tick** (measured and printed by each run). For in-process operations lasting a few hundred nanoseconds, p50/p95/p99 are accurate to about one tick; throughput is not affected.
- **The machine is not isolated.** It is a laptop; client and server share its cores in the networked runs, and memory-bound results vary between runs.
- **Networked runs use loopback**, which excludes the NIC and the physical network.
- **Synthetic key distributions.** 80/20 skew and uniform; neither models a hot set that moves over time.
- **Snapshot loads read a file that was just written**, so they hit the page cache rather than the disk.

Profiling uses macOS `sample` on a running suite. It is not part of `run_all.sh`
and its output is not committed.

---

## 14. Performance results

Every number here is taken from `benchmarks/results/`. The machine is an Apple
M2 Pro (12 cores) with 16 GB of RAM, running macOS 26.5.2, with Apple clang
21.0.0 and a Release build of commit `1ec840b` with no uncommitted changes. The
1-minute load average was 2.10 when the run started. Suite figures are medians
of five runs. The other benchmarks ran once, with the internal repeats noted.
The complete tables are in [benchmarks/RESULTS.md](benchmarks/RESULTS.md).

### Summary

| Measurement | Result |
| --- | --- |
| Single-threaded `Cache`, read-heavy workload | 5,147,260 ops/sec |
| `GET` over loopback TCP, one client | 47,684 req/sec, p50 20.25 µs, p99 34.12 µs |
| `GET` over TCP, 16 clients | 124,904 req/sec (2.55× one client) |
| Sharded against global mutex, 4 threads, read-heavy | +195.6% throughput, −72.7% p99 |
| Hit-rate cost of per-shard LRU | at most 0.02 percentage points |
| `get_into()` against `get()` | +43.6% throughput |
| Memory per entry (16-byte key, 64-byte value) | 221 bytes |
| Snapshot of 500,000 entries | save 281.53 ms, load 259.24 ms, 90 bytes per entry |

### Single-threaded cache

`cachex_bench`, 16-byte keys and 64-byte values.

| Workload | ops/sec | Hit rate | Evictions |
| --- | ---: | ---: | ---: |
| 90% GET / 10% SET, 80/20 skew, capacity 40% of keys | 6,917,673 | 84.21% | 31,495 |
| 20% GET / 80% SET, uniform, capacity 5% of keys | 4,557,396 | 4.92% | 189,939 |

Hit rate against capacity on the skewed workload:

| Capacity | 1% | 5% | 10% | 20% | 40% | 100% |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Hit rate | 3.32% | 15.92% | 30.92% | 57.78% | 84.21% | 100.00% |

40% of the keys serve 84% of requests, well above what a random eviction choice
would give. The uniform workload has no hot set, so its hit rate (4.92%) is
close to the fraction of keys that fit (5%).

Two small costs were measured with paired, order-alternated repeats (15 each):

| Check | Median | Middle half | Full range |
| --- | ---: | --- | --- |
| Capacity check on a cache that never evicts | −0.3% | −0.8% to +0.6% | −20.7% to +23.1% |
| TTL check on a `GET` hit with a deadline | +10.6% | +8.9% to +12.0% | +4.2% to +33.5% |

The capacity check is below the noise floor. The TTL check is measurable:
197.7 ns without a deadline against 218.6 ns with one, roughly the cost of one
`steady_clock::now()` call. Entries without a TTL never read the clock.

### Cache versions under concurrency

`cachex_bench_suite`, median ops/sec of five runs:

| Workload | A, 1 thread | B, 1 thread | B, 4 threads | C, 1 thread | C, 4 threads | C, 16 threads |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| read-heavy | 5,147,260 | 4,731,847 | 1,154,025 | 5,095,958 | 2,869,423 | 2,745,281 |
| balanced | 5,753,640 | 5,472,118 | 1,022,205 | 3,999,064 | 2,988,606 | 2,634,165 |
| write-heavy | 5,023,840 | 3,986,077 | 982,000 | 3,629,953 | 2,994,311 | 2,590,795 |
| high-churn | 3,097,957 | 3,270,561 | 812,551 | 2,703,744 | 2,162,067 | 1,720,683 |
| ttl-heavy | 4,998,774 | 4,215,533 | 1,095,001 | 4,678,912 | 2,889,600 | 2,617,022 |

C against B at the same thread count. Each cell is the median of the five
per-run comparisons.

| Throughput | 1 thread | 2 threads | 4 threads | 8 threads | 16 threads |
| --- | ---: | ---: | ---: | ---: | ---: |
| read-heavy | −10.4% | +20.5% | +195.6% | +67.9% | +57.5% |
| balanced | −12.6% | −2.7% | +173.3% | +85.6% | +58.6% |
| write-heavy | −8.9% | +41.8% | +221.9% | +85.9% | +59.0% |
| high-churn | −13.1% | +11.7% | +150.2% | +75.4% | +51.2% |
| ttl-heavy | −10.9% | +41.7% | +156.2% | +105.9% | +50.7% |

| p99 latency | 1 thread | 2 threads | 4 threads | 8 threads | 16 threads |
| --- | ---: | ---: | ---: | ---: | ---: |
| read-heavy | +9.8% | −55.1% | −72.7% | −45.5% | −46.1% |
| balanced | +26.7% | −42.0% | −71.7% | −51.1% | −66.6% |
| write-heavy | +25.0% | −64.5% | −74.6% | −51.1% | −60.8% |
| high-churn | +30.9% | −65.7% | −70.4% | −54.9% | −71.2% |
| ttl-heavy | +5.9% | −64.3% | −67.5% | −58.4% | −59.5% |

What the data shows:

- **Sharding removes most lock contention.** At 4 threads C beat B in all 25
  runs, by at least +108.8% in the worst run. p99 fell by about 70%.
- **The global mutex is at its worst at 4 threads.** B's throughput is lowest
  there in every workload and partly recovers at 8 and 16 threads. The data does
  not explain why.
- **One shard lock costs something when there is no contention.** At 1 thread
  C is about 9–13% slower than B. The likely reason is that C hashes each key
  twice, once to choose a shard and once in the shard's map. At 1 thread the p99
  differences are a few 41 ns clock ticks.
- **No multi-threaded configuration is faster than the single-threaded
  baseline.** A cache operation takes a few hundred nanoseconds, and
  coordinating threads over shared memory costs more than parallelism returns
  at this size. Sharding's benefit is that concurrent access degrades far less.
- **Single-thread runs are noisy on this machine.** Read-heavy A ranged from
  3.69M to 6.29M ops/sec across the five runs, and in high-churn B's median is
  above A's even though B does strictly more work. Differences of 10–20% between
  single-threaded medians are within run-to-run variation.
- Every run of every configuration had zero errors. Within a workload, hit rates
  differ by at most 0.3 percentage points between versions and thread counts,
  which confirms that each replayed the same operations.

### Memory per entry

| Entries | Heap in use | Bytes per entry | Against the 80-byte payload |
| ---: | ---: | ---: | ---: |
| 100,000 | 20.6 MiB | 216.4 | 2.70× |
| 250,000 | 52.7 MiB | 221.2 | 2.76× |
| 500,000 | 105.5 MiB | 221.2 | 2.76× |

Identical in all five runs. The overhead beyond the payload is two node
allocations per entry, the value's heap buffer, the duplicated key and the
bucket array. Allocator slack within a size class is not included.

### `get()` against `get_into()`

Single thread, read-heavy workload, 64-byte values:

| Variant | ops/sec | Range | Average | p50 | p99 |
| --- | ---: | --- | ---: | ---: | ---: |
| `get()` | 5,343,209 | 3.02M–5.50M | 187.2 ns | 125 ns | 459 ns |
| `get_into()` | 7,671,770 | 4.89M–7.89M | 130.3 ns | 83 ns | 416 ns |

Paired within each run, the improvement was +43.6%, +62.2%, +52.2%, +35.2% and
+43.4%, a median of **+43.6%**. A 64-byte value is past the small-string limit,
so each `get()` hit allocates and frees a string. `get_into()` reuses one buffer.

### Over TCP

`cachex_net_bench` over loopback with `TCP_NODELAY`. Single client, 20,000
requests per command, one request in flight:

| Command | req/sec | Average | p50 | p95 | p99 |
| --- | ---: | ---: | ---: | ---: | ---: |
| `PING` | 51,007 | 19.61 µs | 19.67 µs | 23.00 µs | 28.75 µs |
| `SET` | 47,636 | 20.99 µs | 20.29 µs | 25.04 µs | 32.79 µs |
| `GET` | 47,684 | 20.97 µs | 20.25 µs | 26.62 µs | 34.12 µs |

`PING` touches no cache data, so it is the cost of the transport alone. At the
median, a `GET` adds 0.58 µs and a `SET` 0.62 µs to a round trip of about
20 µs: the cache is roughly 3% of a request.

`GET` with 32,000 requests split across N concurrent clients:

| Clients | 1 shard | 2 shards | 4 shards | 8 shards | Best against 1 shard |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 49,006 | 48,371 | 47,346 | 47,013 | +0.0% |
| 2 | 87,259 | 85,271 | 85,085 | 84,495 | +0.0% |
| 4 | 91,564 | 89,320 | 91,368 | 91,754 | +0.2% |
| 8 | 121,260 | 118,433 | 122,725 | 117,651 | +1.2% |
| 16 | 124,904 | 124,826 | 125,165 | 123,113 | +0.2% |

Throughput at 16 clients is 2.55–2.64× that of one client, whatever the shard
count, and sharding changes it by at most 1.2%. Sixteen client threads and
sixteen workers share 12 cores. The lock is not the limit, so removing it
changes nothing visible. The `SET` run was noisier. Two 8-shard cells fell well
below the rest (57,546 req/sec at 2 clients and 100,762 at 16), and 4 shards led
by 8.7% at 16 clients, with no consistent direction by shard count.

### Sharding without the network

N threads calling `ShardedCache::get()` directly, 600,000 calls per cell:

| Threads | 1 shard | 2 shards | 4 shards | 8 shards | Best against 1 shard |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 6,277,614 | 6,285,983 | 6,535,049 | 6,351,340 | +4.1% |
| 2 | 2,855,990 | 3,327,728 | 3,163,020 | 4,030,134 | +41.1% |
| 4 | 1,666,489 | 2,689,457 | 3,390,673 | 4,584,879 | +175.1% |
| 8 | 2,977,744 | 1,961,763 | 2,887,438 | 4,193,375 | +40.8% |
| 16 | 2,933,021 | 2,040,351 | 2,656,544 | 4,105,306 | +40.0% |

With the transport removed, the lock is the bottleneck and 8 shards give +175%
at 4 threads. Seen together, the TCP and in-process tables show that an
optimisation only matters if it targets the component that is limiting
throughput.

Hit rate with a global LRU (1 shard) against per-shard LRU, same skewed workload:

| Capacity | 1 shard | 2 shards | 4 shards | 8 shards | Worst cost |
| --- | ---: | ---: | ---: | ---: | ---: |
| 5% | 15.92% | 15.91% | 15.93% | 15.90% | −0.01 pp |
| 10% | 30.92% | 30.92% | 30.91% | 30.93% | −0.02 pp |
| 20% | 57.78% | 57.79% | 57.81% | 57.78% | 0.00 pp |
| 40% | 84.21% | 84.21% | 84.21% | 84.20% | −0.01 pp |

### Persistence

`cachex_persist_bench`, 8 shards, median of 5 repeats, snapshot in `/tmp`:

| Entries | Save | Load | Snapshot | Bytes per entry |
| ---: | ---: | ---: | ---: | ---: |
| 1,000 | 0.97 ms | 0.50 ms | 87.9 KiB | 90.0 |
| 10,000 | 4.32 ms | 4.56 ms | 878.9 KiB | 90.0 |
| 100,000 | 51.25 ms | 46.40 ms | 8,789.1 KiB | 90.0 |
| 500,000 | 281.53 ms | 259.24 ms | 43,945.4 KiB | 90.0 |

With a TTL on every entry the snapshot is 95 bytes per entry, and 500,000
entries save in 261.85 ms and load in 300.32 ms.

- Save and load scale linearly, at roughly 0.5 µs per entry.
- The snapshot is 90 bytes per entry against 221 in memory, because the index,
  the list pointers and the duplicated key are rebuilt on load.
- A `SAVE` of 500,000 entries holds the requesting client for about 280 ms.
  Other clients keep being served, because the file is written with no lock
  held.
- The load reads a file that was just written, so it measures parsing and
  inserting, not a cold disk.

---

## 15. Tradeoffs

| Decision | Benefit | Cost |
| --- | --- | --- |
| Hash map plus `std::list` for LRU | O(1) lookup, reorder and eviction; exact LRU | two allocations per entry, the key stored twice, pointer chasing (§14 memory) |
| `get()` returns a copy | no dangling references after eviction | one allocation per hit; `get_into()` avoids it |
| Lazy expiration | no background thread; O(1) work only on entries already found | expired keys that are never touched keep memory and capacity |
| Monotonic clock for expiry | immune to wall-clock jumps | deadlines cannot be serialised; snapshots need a wall-clock anchor |
| Line-based text protocol | readable, testable with `netcat` | keys and values cannot contain whitespace |
| Thread per connection | simple, easy to reason about | a stack and a scheduler slot per client; capped at 256 |
| Exclusive lock for reads | correct under LRU, where reads mutate | readers cannot share a lock |
| One mutex (`SyncCache`) | smallest possible correct design | the cache becomes one sequential section |
| Sharding | independent locks; far less contention | per-shard LRU; aggregate counts are approximate under load |
| Snapshot persistence | simple, atomic, survives planned restarts | full rewrite per save; no crash durability; not a point in time |
| No external dependencies | clone and build with a compiler and CMake | a minimal test framework instead of GoogleTest |

---

## 16. Failure modes

| Failure | Behaviour |
| --- | --- |
| Client disconnects | `recv()` returns 0; the worker closes the socket and exits |
| Client disconnects mid-reply | `send()` fails without `SIGPIPE`; the connection is abandoned; the server continues |
| Malformed or unknown command | `-ERR` reply; the connection stays open |
| Request line over 64 KiB | `-ERR line too long`; the connection closes |
| More than 256 connections | `-ERR server at connection limit`; the new connection closes |
| Operating system refuses a new thread | error logged; that connection closes; the server continues |
| Port already in use | startup fails with a message naming the port (6379 is also Redis's default) |
| Invalid command-line argument | usage message, exit code 2 |
| Snapshot file missing | normal first start; the cache starts empty |
| Snapshot corrupt, truncated, or claiming impossible sizes | rejected with a reason; the cache is unchanged; at startup the server starts empty |
| Disk full or unwritable path during save | save fails, the temporary file is removed, the previous snapshot is untouched |
| Crash during a save | the previous snapshot is intact; the rename never happened |
| Crash between saves | changes since the last save are lost |
| Wall clock moves backwards before a load | elapsed time is clamped at zero; TTLs are never extended |
| Keys expire but are never read | memory and capacity stay occupied until the key is touched or evicted |
| Adversarial keys that collide in one bucket | lookups degrade to O(n); CacheX assumes a trusted network |
| Two clients read, modify and write one key | one update can be lost; commands are atomic, sequences are not |

---

## 17. Future improvements

In order of expected value, based on §14.

1. **Event-loop I/O.** Serve many connections from a few threads with `kqueue`/`epoll`. Over TCP the transport, not the cache, limits throughput, so this addresses the measured bottleneck.
2. **Active expiry.** A per-shard sweep under that shard's lock, sized from measurements of how much expired data accumulates.
3. **Crash-safe snapshots.** `fsync` the temporary file and its directory before renaming, and add a checksum per record.
4. **Length-prefixed protocol.** Allow any bytes in keys and values.
5. **Lower memory per entry.** Store a pointer to the map's key in the list node instead of a second copy, or move to an open-addressing table with an intrusive list.
6. **Compound atomic commands.** `INCR`, compare-and-set, `GETSET`, so clients need not read and write separately.
7. **Heterogeneous lookup.** Look keys up by `std::string_view` to avoid copying each key out of the request.
8. **Better measurement.** Sub-tick latency by batching identical operations, cold-cache snapshot loads, and runs on an isolated machine.

---

## Appendix: interview questions

**Why a hash map and a linked list, not one of them?**
The map finds a key in O(1) but has no order, so finding the least recently used
entry would be O(n). The list maintains order and gives the oldest entry in O(1)
but cannot search. Storing list iterators in the map gives O(1) for both.

**Why doubly linked?**
Removing a node needs its predecessor. A singly linked list must walk from the
head to find it.

**Why does the node store its key when the map already has it?**
Eviction starts from the list's tail. Erasing the map entry needs the key;
without it the map would have to be scanned.

**What happens on GET?**
Hash lookup; if the entry has expired it is removed and the reply is a miss;
otherwise the node is spliced to the head and the value copied out.

**What happens at capacity?**
The new entry is inserted at the head, then the tail entry is removed from the
map and the list. All O(1).

**Why does GET need an exclusive lock?**
Under LRU it moves the entry in the list, so concurrent reads are concurrent
writes to the list.

**Race condition or deadlock?**
A race is unsynchronised access whose result depends on timing. A deadlock is
threads waiting on locks held by each other. CacheX avoids deadlock by never
holding two locks at once.

**Mutex or atomic?**
An atomic makes one operation on one variable indivisible; CacheX uses them for
flags and counters. The cache needs a mutex because its invariant spans a map and
a list.

**Why does one global mutex become a bottleneck?**
Every operation must pass through it one at a time, so extra threads only queue
and add handoff cost. Sharding gives each shard its own mutex.

**What does sharding cost?**
LRU becomes per shard, and aggregate counts are approximate under concurrent
writes.

**What happens with 100 concurrent clients?**
100 worker threads share the machine's cores. Throughput plateaus once the cores
are saturated and latency rises with queueing; clients see higher latency, not
errors, until the 256-connection limit.

**Why can `recv()` return partial data, and why is framing necessary?**
TCP delivers a byte stream without message boundaries, so a read can return any
amount. The protocol's newline framing tells the server where each command ends.

**`listen()` versus `accept()`?**
`listen()` marks the socket passive and lets the kernel queue completed
handshakes. `accept()` takes one queued connection and returns a new socket.

**What happens during `connect()`?**
The two kernels perform the SYN, SYN-ACK, ACK handshake. The server's
application code does not run until it calls `accept()`.

**What happens when a client disconnects?**
`recv()` returns 0. If the server is writing at that moment, `send()` fails;
`SIGPIPE` is suppressed so the process survives.

**TCP or UDP?**
TCP: requests must arrive once and in order, and replies can be larger than one
datagram.

**Why `steady_clock` for TTL?**
It never jumps, so clock corrections cannot resurrect or mass-expire keys.

**How can a snapshot be taken while clients are writing?**
Each shard is locked and copied in turn, so every record is whole; the file is
written after copying, then renamed atomically. The result is consistent per
shard but not a single instant.

**What does persistence not give you?**
Crash durability, `fsync`, checksums and incremental writes. It is for planned
restarts only.

**Why do `SAVE` and `LOAD` take no path?**
A path from the network would let any client read or overwrite arbitrary files.
