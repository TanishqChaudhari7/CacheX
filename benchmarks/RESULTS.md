# Benchmark Results

> **These numbers are specific to one machine and mean nothing in isolation.**
> They exist as a baseline to compare *future versions of CacheX against*, on the
> same hardware, in the same sitting. Do not compare them to numbers from another
> machine, and do not compare them to Redis — Redis pays network and protocol
> costs this in-process benchmark does not.

Methodology: [ARCHITECTURE.md §13](../ARCHITECTURE.md#13-benchmark-methodology).

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DCACHEX_BUILD_BENCHMARKS=ON
cmake --build build/release -j
./build/release/bin/cachex_bench
```

---

## Stage 9 — benchmark suite and profiling

Raw output for everything below is committed in `results/`. Regenerate with
`./run_all.sh`; `results/environment.txt` records the machine, the commit and the
load average at capture time.

| | |
| --- | --- |
| Binary | `cachex_bench_suite` |
| Runs | 5, median reported, range given where wide |
| Workloads | read-heavy, balanced, write-heavy, high-churn, ttl-heavy |
| Versions | A = `Cache` (no locking), B = `SyncCache` (global mutex), C = `ShardedCache(8)` |
| Operations | 400,000 per workload, seed 20260911, replayed identically by each version |

### read-heavy, median of 5

| version | threads | ops/sec | range | p50 | p95 | p99 | hit % |
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

Zero errors and ~44,200 evictions everywhere. Hit rate spans 78.91–78.98% across
all eleven configurations and all five runs — the check that every version really
did receive the identical workload.

### C vs B at matched thread counts (median of 5)

Throughput:

| workload | 1t | 2t | 4t | 8t | 16t |
| --- | ---: | ---: | ---: | ---: | ---: |
| read-heavy | −8.8% | +28.6% | **+174.9%** | +69.3% | +46.9% |
| balanced | −13.1% | +16.3% | **+197.0%** | +56.2% | +57.1% |
| write-heavy | −8.3% | +22.1% | **+178.7%** | +94.1% | +67.0% |
| high-churn | −11.0% | +29.1% | **+197.7%** | +67.1% | +35.9% |
| ttl-heavy | −4.3% | +44.5% | **+234.6%** | +66.4% | +57.0% |

p99 latency (negative is better):

| workload | 1t | 2t | 4t | 8t | 16t |
| --- | ---: | ---: | ---: | ---: | ---: |
| read-heavy | +9.8% | −59.5% | **−69.3%** | −36.7% | −33.1% |
| balanced | +13.3% | −55.8% | **−72.7%** | −32.8% | −46.8% |
| write-heavy | +9.8% | −60.4% | **−69.6%** | −54.4% | −68.8% |
| high-churn | +13.3% | −66.8% | **−75.2%** | −49.5% | −62.2% |
| ttl-heavy | +9.2% | −62.1% | **−76.1%** | −38.6% | −56.4% |

### Optimisation: `get_into()`

Profiling (`sample`, 8 s) ranked mutex contention first (~13,700 samples) and
malloc/free next. `get()` returns `optional<string>`, so every hit on a 64-byte
value is one malloc and one free. `get_into()` assigns into a caller-owned buffer.

| variant | ops/sec (median) | range | avg ns | p50 |
| --- | ---: | --- | ---: | ---: |
| `get()` | 5,118,081 | 4.47 M – 5.41 M | 195.4 | 125 |
| `get_into()` | **7,634,589** | 6.05 M – 7.85 M | 131.0 | 83 |

**Median +45.3%**, per-run range **+18.2% to +72.9%**. The spread is machine
noise: the two variants run back to back, and a busy machine sinks the `get()`
baseline further than `get_into()`, inflating the ratio. The claim worth making is
the median, with +18% as the worst actually observed.

### Memory per entry — measured

| entries | heap delta | bytes/entry | vs payload |
| ---: | ---: | ---: | ---: |
| 100,000 | 20.6 MiB | 216.4 | 2.70× |
| 250,000 | 52.7 MiB | 221.2 | 2.76× |
| 500,000 | 105.5 MiB | 221.2 | 2.76× |

**221 bytes/entry**, identical in all five runs. This corrects the "~180–200
estimated" figure carried since Stage 3 — the real number is higher.

### Two measurement bugs, both caught by an impossible result

1. **Memory measured with RSS** reported **0 bytes/entry** for 100k entries, because the allocator had already grown the heap and never returns pages. Fixed by using `malloc` bytes-in-use.
2. **No warm-up before version A**, which ran first in each workload and absorbed the page-fault and allocator cost. That made the *unlocked* baseline measure slower than the mutex version at one thread — impossible, since B does strictly more work. Fixed with a discarded pass per workload.

Neither was found by reading the code. Both were found by noticing a number that
could not be true.

### The headline, unflattering

**No multi-threaded configuration beats the single-threaded baseline.** A
cache operation costs ~170 ns and touches shared memory; thread coordination
costs more than the parallelism buys. Sharding's measured value is beating the
global mutex under concurrency, not scaling past one thread.

---

## Stage 8 — persistence

`cachex_persist_bench`, Release, 16-byte keys / 64-byte values, 8 shards, median
of 5 runs, on this machine's `/tmp`.

### Entries without a TTL

| entries | save | load | save µs/entry | snapshot | bytes/entry |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1,000 | 0.75 ms | 0.52 ms | 0.751 | 87.9 KiB | 90.0 |
| 10,000 | 4.55 ms | 4.67 ms | 0.455 | 878.9 KiB | 90.0 |
| 100,000 | 50.54 ms | 48.51 ms | 0.505 | 8.6 MiB | 90.0 |
| 500,000 | 264.00 ms | 252.25 ms | 0.528 | 42.9 MiB | 90.0 |

### Entries with a TTL

| entries | save | load | snapshot | bytes/entry |
| ---: | ---: | ---: | ---: | ---: |
| 1,000 | 0.66 ms | 0.59 ms | 92.8 KiB | 95.0 |
| 10,000 | 4.41 ms | 5.74 ms | 927.8 KiB | 95.0 |
| 100,000 | 53.30 ms | 57.00 ms | 9.1 MiB | 95.0 |
| 500,000 | 302.23 ms | 294.02 ms | 45.3 MiB | 95.0 |

A TTL costs **5 bytes per entry** — the millisecond count in place of `-1`.

### Reading these

**Save and load both scale linearly**, at roughly **0.5 µs per entry** each way.
That is what a single pass over the data should look like: the save walks every
entry once and writes it; the load parses every record once and calls `set()`.
There is no index to rebuild on disk and none to sort on the way in.

Load is consistently a little *faster* than save at the same size despite doing
more work per record (parsing, then a hash insert and an LRU splice). The likely
reason is the page cache — the file was just written, so the read never touches
the device. A cold-cache load would be slower, and this benchmark does not
measure that.

### Snapshot size against memory

| | bytes/entry | vs payload |
| --- | ---: | ---: |
| Payload (key + value) | 80 | 1.00x |
| Snapshot on disk | 90 | 1.13x |
| Live cache in memory | **221 (measured)** | 2.76x |

The snapshot is **roughly half the size of the live cache**. It stores only the
data; the hash map's buckets and nodes, both list pointers, the duplicated key
and the allocator's overhead exist to make lookups O(1), and none of that is
worth writing down because loading rebuilds it.

The 10 bytes of snapshot overhead per entry are the length header
(`<key_len> <value_len> <ttl_ms>\n`) plus the trailing newline. The in-memory
figure was an estimate when this was written; **Stage 9 measured it at 221
bytes/entry**, so the snapshot is ~2.5× smaller than the live cache rather than
the ~2× originally suggested.

### The number with an operational consequence

**A synchronous `SAVE` of 500,000 entries blocks the connection that issued it
for ~264 ms.** Other clients are unaffected: `save()` copies the cache out under
per-shard locks and then writes with no lock held, so the disk never stalls the
request path. But the client that asked waits for the whole write.

That is the argument for `BGSAVE` (fork, let the child write a copy-on-write
snapshot) at a size where 264 ms matters — and the reason it is not here is that
it is a great deal more machinery than this stage calls for.

---

## Stage 7 — sharding

| | |
| --- | --- |
| Date | 2026-09-11 |
| Machine | Apple M2 Pro, 12 cores / 12 hardware threads, 16 GB RAM |
| OS | macOS 26.5.2 (arm64) |
| Compiler | AppleClang 21.0.0, `-O3 -DNDEBUG`, C++17 |
| Configurations | A = 1 shard (one global mutex), B = 2, C = 4, D = 8 |
| Work | 32,000 requests per networked config; 600,000 per in-process config |

Identical workload and machine conditions across every configuration. Full
interpretation, including what sharding does and does not fix, is in
[ARCHITECTURE.md → Measured Performance Improvements](../ARCHITECTURE.md#measured-performance-improvements).

### Headline

| | result |
| --- | --- |
| In-process, 4 threads, 8 shards vs 1 | **+232.0%** throughput (2.25M → 7.48M ops/sec) |
| In-process, 1 thread | **+0.0%** — no contention to remove |
| Over TCP, any client count | **≤ +0.8%** throughput, ≤2% p99 — inside noise |
| Hit-rate cost of per-shard LRU | **≤ 0.02 percentage points** |

The 4-thread in-process figure reproduced across three runs at **+227%, +232%,
+240%**. The 1-thread figure reproduced at +0.0% every time.

### Raw benchmark output

```
SHARDING OVER TCP
=================
32000 requests per configuration, split across N clients.
Version A = 1 shard (one global mutex), B = 2, C = 4, D = 8.
Clients and server share this machine's 12 hardware threads.

GET (all hits) -- throughput (req/sec)

clients         1 shard     2 shards     4 shards     8 shards       best vs 1
------------------------------------------------------------------------------
1                 48510        48499        48469        48005           +0.0%
2                 85206        84637        83432        84186           +0.0%
4                 89629        89849        88865        90380           +0.8%
8                122006       121745       121547       122563           +0.5%
16               124484       124782       123071       123028           +0.2%

GET (all hits) -- p99 latency (us)

clients         1 shard     2 shards     4 shards     8 shards       best vs 1
------------------------------------------------------------------------------
1                 28.21        28.71        28.12        27.62           -2.1%
2                 36.38        36.38        40.79        37.29           +0.0%
4                 56.38        57.38        56.04        56.50           -0.6%
8                 84.12        84.83        83.38        82.71           -1.7%
16               148.42       149.62       170.12       176.75           +0.0%

GET (all hits) -- scaling vs 1 client, per shard count

clients         1 shard     2 shards     4 shards     8 shards
--------------------------------------------------------------
1                 1.00x        1.00x        1.00x        1.00x
2                 1.76x        1.75x        1.72x        1.75x
4                 1.85x        1.85x        1.83x        1.88x
8                 2.52x        2.51x        2.51x        2.55x
16                2.57x        2.57x        2.54x        2.56x

SET -- throughput (req/sec)

clients         1 shard     2 shards     4 shards     8 shards       best vs 1
------------------------------------------------------------------------------
1                 47916        47383        48286        48010           +0.8%
2                 84041        76037        84802        85789           +2.1%
4                 90450        90594        91204        90679           +0.8%
8                120010       121023       121326       119925           +1.1%
16               123277       123139       123357       123094           +0.1%

SET -- p99 latency (us)

clients         1 shard     2 shards     4 shards     8 shards       best vs 1
------------------------------------------------------------------------------
1                 28.83        32.67        29.33        28.33           -1.7%
2                 38.75        50.83        36.50        35.75           -7.7%
4                 57.79        57.29        57.08        56.54           -2.2%
8                 86.38        83.79        82.38        88.75           -4.6%
16               155.21       152.21       152.75       155.25           -1.9%

SET -- scaling vs 1 client, per shard count

clients         1 shard     2 shards     4 shards     8 shards
--------------------------------------------------------------
1                 1.00x        1.00x        1.00x        1.00x
2                 1.75x        1.60x        1.76x        1.79x
4                 1.89x        1.91x        1.89x        1.89x
8                 2.50x        2.55x        2.51x        2.50x
16                2.57x        2.60x        2.55x        2.56x


SHARDING IN-PROCESS (no sockets)
================================
N threads calling ShardedCache::get() directly, 600000 total calls.

threads         1 shard     2 shards     4 shards     8 shards       best vs 1
------------------------------------------------------------------------------
1               7146193      6885913      6941097      6855821           +0.0%
2               3853705      3837794      4093006      4882059          +26.7%
4               2252335      3376833      4501711      7477789         +232.0%
8               3289281      2466958      3060406      4685978          +42.5%
16              3205524      2243006      2855006      4103501          +28.0%


GLOBAL LRU vs PER-SHARD LRU (hit rate cost of sharding)
======================================================
Skewed 80/20 workload, cache-aside, single-threaded so only
the eviction policy differs.

capacity          1 shard     2 shards     4 shards     8 shards          cost
------------------------------------------------------------------------------
5% (5000)          15.92%       15.91%       15.93%       15.90%        -0.01pp
10% (10000)        30.92%       30.92%       30.91%       30.93%        -0.02pp
20% (20000)        57.78%       57.79%       57.81%       57.78%        +0.00pp
40% (40000)        84.21%       84.21%       84.21%       84.20%        -0.01pp

  'cost' is the worst shard count's hit rate minus the
  1-shard (true global LRU) hit rate, in percentage points.

  (checksum 768000000)
```

### Why the two results differ

They are the same finding seen through different bottlenecks.

Stage 5 measured a TCP round trip at ~20 µs against a ~0.4 µs cache operation, and
Stage 6's `PING` control — which takes no lock at all — plateaued exactly where
`GET` did. The transport, syscalls and scheduler cap throughput long before the
cache mutex does. Removing contention from a 2% slice of the request cannot move
the total, no matter how completely it is removed.

Strip the network away and the lock is the only thing left, which is why the
in-process numbers are dramatic and the networked ones are flat.

**The practical lesson is the one that transfers:** an optimisation that is
spectacular in a microbenchmark and invisible end to end has optimised something
that was not the constraint. The only way to know which you have is to measure
both, which is why this benchmark reports both.

### An oddity, flagged rather than explained away

In the in-process table the **1-shard column rises** from 4 to 8 threads
(2.25M → 3.29M ops/sec). More contention should not be faster. The likely cause
is lock-handoff batching — under heavy contention a thread that releases and
immediately reacquires keeps the cache line locally, cutting cross-core traffic.
That was not verified, so it is a hypothesis, not a finding.

---

## Stage 6 — concurrency

| | |
| --- | --- |
| Date | 2026-09-11 |
| Machine | Apple M2 Pro, 12 cores / 12 hardware threads, 16 GB RAM |
| OS | macOS 26.5.2 (arm64) |
| Compiler | AppleClang 21.0.0, `-O3 -DNDEBUG`, C++17 |
| Server | Thread-per-connection, one `std::mutex` around the cache |
| Work | 32,000 total requests per configuration, split evenly across clients |

All clients are released simultaneously by a start gate, so a "16 client" run
really does have 16 clients in flight rather than a staggered ramp.

### Scaling, 1 → 16 concurrent clients

`scaling` = throughput(N clients) / throughput(1 client). Perfect would be N.

**PING** — the control. Takes no lock, touches no cache data.

| clients | per client | req/sec | scaling | p50 | p95 | p99 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 32,000 | 51,384 | 1.00x | 19.67 µs | 22.33 µs | 27.83 µs |
| 2 | 16,000 | 85,542 | 1.66x | 22.62 µs | 30.96 µs | 37.54 µs |
| 4 | 8,000 | 90,554 | 1.76x | 46.12 µs | 51.50 µs | 57.21 µs |
| 8 | 4,000 | 123,614 | 2.41x | 64.00 µs | 77.42 µs | 88.75 µs |
| 16 | 2,000 | 126,149 | 2.46x | 126.46 µs | 140.04 µs | 146.54 µs |

**GET** (all hits) — takes the lock.

| clients | per client | req/sec | scaling | p50 | p95 | p99 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 32,000 | 48,228 | 1.00x | 20.17 µs | 23.67 µs | 30.33 µs |
| 2 | 16,000 | 85,435 | 1.77x | 22.92 µs | 31.58 µs | 37.33 µs |
| 4 | 8,000 | 90,826 | 1.88x | 45.50 µs | 51.62 µs | 57.79 µs |
| 8 | 4,000 | 121,930 | 2.53x | 65.00 µs | 76.92 µs | 83.88 µs |
| 16 | 2,000 | 122,507 | 2.54x | 128.96 µs | 149.21 µs | 165.62 µs |

**SET** — takes the lock. Distinct key space per client, so this measures
contention rather than clients overwriting each other.

| clients | per client | req/sec | scaling | p50 | p95 | p99 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 32,000 | 46,638 | 1.00x | 20.12 µs | 24.75 µs | 30.62 µs |
| 2 | 16,000 | 84,821 | 1.82x | 22.67 µs | 31.79 µs | 38.04 µs |
| 4 | 8,000 | 91,909 | 1.97x | 44.79 µs | 52.12 µs | 58.88 µs |
| 8 | 4,000 | 119,416 | 2.56x | 64.96 µs | 79.96 µs | 90.12 µs |
| 16 | 2,000 | 124,034 | 2.66x | 127.71 µs | 141.25 µs | 148.83 µs |

Zero errors in every configuration.

### ⭐ The mutex is not the bottleneck here — and here is the proof

The tempting conclusion from "2.5x at 16 clients" is *the global mutex is
serialising everything*. **That is wrong, and the PING control is why.**

`PING` takes no lock at all and touches no cache data, yet it plateaus at 2.46x —
within noise of GET's 2.54x and SET's 2.66x. If the cache mutex were the limit,
the lock-free path would have kept scaling. It did not.

This follows from Stage 5's measurement that a round trip is ~20 µs while the
cache operation is ~0.4 µs. A 2% serial section cannot cap speedup at 2.5x;
Amdahl's law would allow ~50x. The ceiling is the transport, the syscalls, and a
scheduler running ~32 threads (16 clients + 16 workers) on 12 hardware threads.

### But remove the network, and the mutex has nowhere to hide

N threads calling `SyncCache::get()` directly, no sockets:

| threads | ops/sec | scaling | ns/op |
| ---: | ---: | ---: | ---: |
| 1 | 4,391,985 | 1.00x | 227.7 |
| 2 | 3,655,803 | 0.83x | 273.5 |
| 4 | 2,122,690 | 0.48x | 471.1 |
| 8 | 2,691,356 | 0.61x | 371.6 |
| 16 | 3,065,257 | 0.70x | 326.2 |

**Adding threads makes it slower.** Throughput never reaches the single-threaded
figure, because the lock admits exactly one thread at a time and the extra threads
contribute only contention and handoff cost. That is what a hard sequential
section looks like, and it is the real argument for sharding: not that the lock is
slow today, but that it is a wall waiting to be hit.

### The third signature: saturation

Across all three TCP tables, **p50 latency rises roughly in proportion to client
count (20 µs → 128 µs, ~6.4x for 16x the clients) while throughput stays flat**.
Growing queue, constant service rate — the textbook signature of a saturated
resource. Clients experience saturation as latency, not as errors, which is why
there are zero errors in every row.

### What to do with these numbers

They are the honest answer to "how far does thread-per-connection plus one mutex
get you": **~120k req/sec and ~2.5x scaling on this machine**, limited by the
transport rather than by the cache. Any future claim of improvement has to beat
those figures on the same machine in the same sitting.

Two things would move them, and the data says which to do first:

1. **A better I/O model** (an event loop instead of a thread per connection) attacks the measured bottleneck.
2. **Sharding the cache** attacks the bottleneck that the in-process table shows is waiting behind it.

Doing 2 before 1 would be optimising the part that is not currently limiting
anything — which is exactly the mistake this benchmark was built to prevent.

---

## Stage 5 — over TCP

| | |
| --- | --- |
| Date | 2026-09-11 |
| Machine | Apple M2 Pro, 12 cores, 16 GB RAM |
| OS | macOS 26.5.2 (arm64) |
| Compiler | AppleClang 21.0.0, `-O3 -DNDEBUG`, C++17 |
| Harness | `cachex_net_bench` — server in-process on a thread, loopback, `TCP_NODELAY` |
| Client | 1, blocking, one request in flight (no pipelining) |

```
phase       requests       req/sec    avg (us)        p50        p95        p99    max (us)
-------------------------------------------------------------------------------------------
PING           20000         50114       19.95      19.75      24.79      30.83      195.33
SET            20000         47482       21.06      20.29      26.25      32.50      230.46
GET            20000         48450       20.64      20.17      25.46      31.58      205.54
```

Three consecutive runs (req/sec): PING 50114 / 49438 / 50056, SET 47482 / 47495 /
48033, GET 48450 / 47182 / 47935 — about 3% spread, noticeably steadier than the
in-process phases, because a round trip is dominated by a fairly uniform
syscall-and-scheduling path rather than by memory access patterns.

### ⭐ The transport is 97–98% of a request

`PING` touches no cache data at all. It is the floor: pure round trip.

| | p50 | what it includes |
| --- | ---: | --- |
| `PING` | 19.75 µs | the transport, and nothing else |
| `GET` | 20.17 µs | transport **+ a cache read** |
| `SET` | 20.29 µs | transport **+ a cache write** |

The cache operation is the difference: **~0.4–0.5 µs out of ~20 µs.**

This cross-validates nicely with the in-process benchmark, which measured a
`GET hit` at ~360–430 ns by a completely different method. Two independent
measurements agreeing to within a rounding error is far better evidence than
either alone.

**The uncomfortable implication is the useful one.** Every nanosecond won in
Stages 2–4 — the O(1) eviction, the `optional` short-circuit that keeps the TTL
check off the no-TTL path — is invisible from the far side of a socket. A
50% faster cache would move this benchmark by about 1%. That is why the next
stages are about concurrency and not micro-optimisation, and it is the argument
for always measuring at the boundary the user actually sees.

### These percentiles are real, unlike the in-process ones

A round trip is ~20 µs against a **41 ns clock tick** — three orders of
magnitude apart. So `p50 = 19.75 µs` is a measurement, not a tick count. Compare
Stage 4, where the TTL check's 12 ns effect was completely invisible in the
percentile table because the tick was three times larger than the effect.

Same harness code, same clock; what changed is the ratio between the thing being
measured and the instrument measuring it. That ratio is the whole question when
deciding whether a percentile means anything.

### What this baseline is for

It is deliberately the *most pessimistic* configuration: one client, one request
in flight, no pipelining, a server that handles one connection at a time. Every
later stage gets compared against it:

| | Status |
| --- | --- |
| single client → this baseline | ✅ recorded |
| multiple clients against the single-threaded server | ⬜ Stage 7 |
| multiple clients against a concurrent server | ⬜ Stage 7 |
| sharded cache under concurrent load | ⬜ Stage 8 |

### Limitations

- **Loopback, not a network.** No NIC, no wire, no switch, and most of the IP stack is short-circuited. A real network adds tens to hundreds of microseconds and would make the cache's share of a request smaller still.
- **Server and client share a machine**, and on an idle one they may well share a core's cache. Both processes' CPU time lands in the same measurement.
- **No pipelining.** The server *does* handle several buffered commands per read, but the benchmark never sends them that way. Pipelining would amortise the round trip across many requests and change these numbers completely.
- **One value size (64 B), one key size (16 B).** Larger values would start to make bandwidth matter.

---

## Stage 4 — TTL (lazy expiration)

| | |
| --- | --- |
| Date | 2026-09-11 |
| Machine | Apple M2 Pro, 12 cores, 16 GB RAM |
| OS | macOS 26.5.2 (arm64) |
| Compiler | AppleClang 21.0.0, `-O3 -DNDEBUG`, C++17 |
| Clock | `steady_clock`, measured: ~21–38 ns per `now()`, **41 ns tick** |

Stage 3's numbers below are unchanged; this section covers what TTL added.

### The cost of the expiry check

The question: how much does checking `expires_at` slow down a `GET` hit? Measured
with the same paired, order-alternated method as the LRU A/B in section 2 —
100,000 GET hits over a shuffled order, 15 repeats per invocation.

```
case                                 ops/sec    avg (ns)     reclaimed
----------------------------------------------------------------------
a) GET hit, no TTL                   9622286       103.9             0
b) GET hit, TTL set                  8598360       116.3             0
c) GET, all entries expired          6348316       157.5        100000
```

Paired `(b)` against `(a)`, **20 invocations** (each a median of 15 paired repeats):

| | |
| --- | ---: |
| Median | **+12.3%** |
| Middle half (p25–p75) | +11.7% to +12.7% |
| Full range | +1.8% to +28.1% |
| Samples above zero | **20 of 20** |

**The TTL check costs about 12% of a GET hit** — roughly 12 ns on a ~104 ns
operation, which is about what a single `steady_clock::now()` read costs. That is
exactly what the check adds when the entry has a deadline.

The number worth trusting most is not the median but the **sign consistency**:
every one of 20 measurements came out positive. A null effect would land negative
roughly half the time. The magnitude estimate is looser than a first batch of five
runs suggested (those happened to fall in a tight +11.7% to +12.7% band and were
not representative of the spread).

**Contrast this with section 2**, where the LRU capacity check straddled zero under
the *identical* method and was reported as unresolvable. Two effects, one method,
two different verdicts — which is the evidence that the harness distinguishes
signal from noise rather than always shrugging.

### Three things this measurement shows

1. **Keys without a TTL pay nothing.** `expires_at` is a `std::optional`, and
   `has_value()` is tested *before* the clock is read. Case (a) is the no-TTL
   path and it is the fastest of the three. The cost is borne only by keys that
   asked for a TTL.

2. **The latency percentiles cannot see this effect at all:**

   ```
     latency (ns)    samples       mean       p50       p95       p99         max
     ----------------------------------------------------------------------------
     no TTL           100000      217.4     208.0     458.0     625.0     13041.0
     TTL set          100000      214.9     208.0     458.0     583.0      7291.0
     expired          100000      229.1     167.0     500.0     708.0      7833.0
   ```

   `p50` reads 208 ns for both cases, because the clock's **41 ns tick** is more
   than three times larger than the 12 ns difference being measured. The
   throughput measurement resolves what the percentile table structurally cannot.
   This is the clearest argument in the project for reporting both.

3. **Walking a cache of entirely expired entries** runs at ~6.3M ops/sec against
   ~9.6M for live hits. That is not the same operation being slower: those calls
   return nothing *and* delete an entry. It is reported to show what a sea of
   expired entries costs to traverse once — and case (c) varies far more between
   runs (183–333 ns/op observed) because it is allocator-teardown bound.

### What is still deterministic

The `reclaimed` count is exactly 100,000 every run, and the checksum is
byte-identical across all 20 invocations. Expiry behaviour is reproducible; only
its timing is not.

---

## Stage 3 — LRU eviction

| | |
| --- | --- |
| Date | 2026-09-11 |
| Machine | Apple M2 Pro, 12 cores, 16 GB RAM |
| OS | macOS 26.5.2 (arm64) |
| Compiler | AppleClang 21.0.0, `-O3 -DNDEBUG`, C++17 |
| CacheX version | 0.1.0 |
| Clock | `steady_clock`, measured: ~21 ns per `now()`, **41 ns tick** |

### What is deterministic, and what is not

Every *behavioural* number the harness reports is byte-identical across runs —
hit rate, miss count, fills, evictions, resident entries, and the checksum:

```
hit rate   : 84.21%  (151475 hits, 28407 misses)     <- identical, all runs
evictions  : 31495,  entries resident: 40000         <- identical, all runs
checksum: 331827648                                  <- identical, all runs
```

Only the *timings* vary. That split is the point: it means a change in hit rate
between two versions of CacheX is a real behavioural change and never noise,
while a change in throughput needs the caveats below.

---

## 1. Core operations (unbounded cache)

Continuity with the Stage 2 baseline — the same six phases, unchanged code paths.
Average latency in ns, across 5 invocations (each a median of 5 internal repeats):

| phase | run 1 | run 2 | run 3 | run 4 | run 5 | spread |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| SET insert | 150.7 | 141.4 | 141.1 | 152.4 | 151.4 | ~8% |
| SET update | 717.3 | 696.2 | 689.7 | 710.4 | 735.9 | ~7% |
| GET hit | 406.6 | 432.2 | 426.8 | 414.6 | 428.9 | ~6% |
| GET miss | 45.9 | 46.1 | 44.3 | 50.1 | 50.0 | ~13% |
| CONTAINS | 49.4 | 48.9 | 51.7 | 58.7 | 57.4 | ~19% |
| ERASE | 353.2 | 363.7 | 393.5 | 347.0 | 396.3 | ~14% |

The relationships from Stage 2 still hold, unchanged by adding LRU:
`CONTAINS ≈ GET miss` (the hash lookup alone), `GET hit` ≈ 8× `CONTAINS` (the
splice plus the 64-byte value copy), and `SET update` ≈ 4.7× `SET insert` despite
doing strictly less work, purely because it walks a shuffled order through
scattered nodes.

---

## 2. What LRU costs

The question: how much does the capacity check slow down a cache that never
actually evicts? Measured by replaying one identical request sequence against an
unbounded cache and a bounded cache with enough headroom that it evicts nothing —
paired within each repeat and with the running order alternated.

Typical run:

```
configuration                            ops/sec    avg (ns)    evictions
-------------------------------------------------------------------------
unbounded (no LRU)                       9485491       105.4            0
bounded, capacity = key space            9415771       106.2            0
bounded, capacity = 10% of keys          7105248       140.7        80196
```

Paired comparison, median across 5 invocations of 15 repeats each:

| run | median | middle half |
| --- | ---: | --- |
| 1 | −5.7% | −13.9% to +5.0% |
| 2 | +1.1% | −5.0% to +7.2% |
| 3 | −1.0% | −9.0% to +4.1% |
| 4 | +6.2% | −3.3% to +11.3% |
| 5 | −4.9% | −10.9% to +20.2% |

**The result straddles zero in every run.** The honest conclusion is that the
cost of the capacity check is *below this harness's noise floor* — which is the
expected outcome for one predictable, almost-never-taken branch per insert.

**This is explicitly not a claim that LRU is free.** It is a statement that the
experiment cannot resolve a cost this small. Claiming a 0% overhead from data
that also "shows" −5.7% and +6.2% would be reading noise as signal.

Two measurement bugs had to be fixed before even this much was trustworthy, and
both are worth knowing about:

- **Comparing two independent medians** let machine drift between the two measurements masquerade as a result. Fixed by pairing the comparison within each repeat.
- **Always running the unbounded case first** made the bounded case look consistently ~8% *faster* — impossible, since it does strictly more work. The first run pays for heap growth that the second then reuses. Fixed by alternating the order between repeats.

The third row is listed but **is not comparable to the other two**: it holds a
tenth of the data, so its memory access pattern differs as much as its workload
does. The ~34% gap there is mostly a different cache-locality regime, not the
cost of eviction.

---

## 3. Workload A — mostly hits (90% GET / 10% SET, cache-aside)

80/20 skewed keys over a 100,000-key space, capacity 40,000, measured at steady
state after a discarded warm-up pass.

```
  requests   : 200000  (179882 GET, 20118 SET)
  throughput : 7012623 ops/sec (142.6 ns/op per application request)
  hit rate   : 84.21%  (151475 hits, 28407 misses)
  fills      : 28407 (cache-aside populations on miss)
  evictions  : 31495,  entries resident: 40000

  latency (ns)    samples       mean       p50       p95       p99         max
  ----------------------------------------------------------------------------
  GET              179882       91.1      83.0     125.0     167.0     18292.0
  SET               48525      204.0     208.0     333.0     375.0      6375.0
```

Throughput across 5 invocations: 6.97M, 6.87M, 6.88M, 6.79M, 6.88M ops/sec —
about 2.6% spread, far tighter than the Stage 2 core phases, because the steady
state avoids the cold-start transient and the map's growth rehashes.

---

## 4. Workload B — heavy churn (20% GET / 80% SET, cache-aside)

Uniform keys over 100,000, capacity 5,000. No hot set exists, so there is nothing
for LRU to retain — the worst case for any eviction policy.

```
  requests   : 200000  (40085 GET, 159915 SET)
  throughput : 4741392 ops/sec (210.9 ns/op per application request)
  hit rate   : 4.92%  (1971 hits, 38114 misses)
  fills      : 38114 (cache-aside populations on miss)
  evictions  : 189939,  entries resident: 5000

  latency (ns)    samples       mean       p50       p95       p99         max
  ----------------------------------------------------------------------------
  GET               40085       63.6      42.0     125.0     125.0      5708.0
  SET              198029      205.6     208.0     292.0     334.0     12041.0
```

Throughput across 5 invocations: 4.60M, 4.61M, 4.63M, 4.68M, 4.49M ops/sec (~4%).

**Churn costs about 1.5× per request** versus the hit-heavy workload (211 ns vs
143 ns). The reason is visible in the counters: 189,939 evictions against 31,495,
and almost every miss pays for a failed lookup, an insert, *and* an eviction.

Note the hit rate of 4.9%, which is roughly capacity ÷ key space (5%). That is
what LRU looks like when the access pattern has no locality to exploit: it
performs no better than chance, because there is no "recently used" signal to
act on. It is not a defect in the implementation — it is the honest answer to
"what does a cache do for a workload that a cache cannot help?"

---

## 5. ⭐ Hit rate vs capacity — the point of LRU

Same request sequence, same skewed distribution, only capacity varies:

| capacity | entries | hit rate | evictions |
| --- | ---: | ---: | ---: |
| 1% | 1,000 | 3.32% | 193,372 |
| 5% | 5,000 | 15.92% | 168,125 |
| 10% | 10,000 | 30.92% | 138,149 |
| 20% | 20,000 | 57.78% | 84,385 |
| 40% | 40,000 | **84.21%** | 31,495 |
| 100% | 100,000 | 100.00% | 0 |

This is the strongest evidence in the project that eviction is choosing the
**right** victims. Hit rate rises much faster than capacity does — 40% of the
memory buys 84% of the hits — because the entries being retained are the ones
actually being requested. A policy that evicted at random would track capacity
roughly linearly and would land near 40% here, not 84%.

It also shows the other half of the story: at 1% capacity the cache thrashes
(193,372 evictions to serve 200,000 requests, 3.3% hit rate). An
undersized LRU cache is close to pure overhead, which is why capacity is a
tuning decision and not a formality.

---

## ⚠️ How much to trust the timings

**Behavioural numbers (hit rate, evictions, fills, checksum) are exact and
reproducible.** Compare them freely between versions.

**Throughput and latency are not.** Two cautions:

1. **Latency percentiles are quantised to the clock's 41 ns tick.** A `p50` of 42 ns means *one tick*, not a 42 ns measurement. The reported p50/p95/p99 are effectively tick counts: 42, 83, 125, 167, 208… are 1, 2, 3, 4, 5 ticks. For operations in this range that is coarse, and finer resolution needs a different technique — Stage 5.

2. **Absolute throughput is only comparable within one sitting.** In Stage 2, across 12 runs of an identical binary, the memory-bound phases fell into two clusters ~2× apart and the cause was never established (CPU contention alone did not explain it). The Stage 3 workload numbers are much steadier (2–4% spread) because of the warm-up, but the underlying machine behaviour has not changed.

The rule: **compare two versions back to back in the same sitting, and trust
ratios between phases over absolute throughput.** Section 2 of the harness is
built to satisfy this — it runs its A/B inside a single process, interleaved and
order-alternated, rather than against a number recorded on another day.

---

## Known limitations

- **Average and tick-quantised percentiles only.** No sub-tick resolution. ⬜ Stage 5.
- **Single-threaded.** No contention, no scaling data. ⬜ Stage 7.
- **One key size, one value size.** 16-byte keys (small-string, no allocation) and 64-byte values (heap-allocated). Behaviour at other sizes is not characterised.
- ~~Memory per entry is not measured~~ — ✅ measured in Stage 9: **221 bytes/entry**.
- **Two synthetic distributions.** 80/20 skew and uniform. Real traffic is Zipfian with a time-varying hot set, and neither of these captures a working set that shifts.
- **In-process.** No sockets, no protocol parsing, no syscalls. The Stage 5 section above measures the same cache over TCP, where these per-operation costs turn out to be 2-3% of a request.
