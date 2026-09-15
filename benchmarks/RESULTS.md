# Benchmark Results

All tables below come from the raw output in [`results/`](results/). Methodology
is in [ARCHITECTURE.md §13](../ARCHITECTURE.md#13-benchmark-methodology), and the
interpretation is in [§14](../ARCHITECTURE.md#14-performance-results).

These numbers are specific to one machine. They are useful for comparing
configurations of CacheX against each other in the same sitting, not for
comparing against other systems or other hardware.

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DCACHEX_BUILD_BENCHMARKS=ON
cmake --build build/release -j
RUNS=5 ./benchmarks/run_all.sh
```

## Environment

From [`results/environment.txt`](results/environment.txt):

| | |
| --- | --- |
| Machine | Apple M2 Pro, 12 cores, 16 GB RAM |
| OS | macOS 26.5.2 (arm64) |
| Compiler | Apple clang 21.0.0, Release (`-O3 -DNDEBUG`), C++17 |
| Commit | `1ec840b`, no uncommitted changes |
| Load average at start | 2.10 (1 min) |
| Clock | `steady_clock`, 41.0 ns tick |

---

## Benchmark suite

`cachex_bench_suite`, run five times
([`bench_suite.1.txt`](results/bench_suite.1.txt) to
[`bench_suite.5.txt`](results/bench_suite.5.txt)). Each cell is the median of the
five runs, and the range is the lowest and highest run. 400,000 operations per
workload, seed 20260911, 16-byte keys and 64-byte values.

| Version | Type | Locking |
| --- | --- | --- |
| A | `Cache` | none, single thread only |
| B | `SyncCache` | one global mutex |
| C | `ShardedCache(8)` | one mutex per shard |

Latency percentiles are quantised to the 41 ns clock tick. Throughput is computed
from wall time and is not.

### read-heavy

90% GET / 10% SET, 80/20 skew over 100,000 keys, capacity 40,000.

| Version | Threads | ops/sec | Range (M) | p50 ns | p95 ns | p99 ns | Hit % |
| --- | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| A `Cache` | 1 | 5,147,260 | 3.69–6.29 | 125 | 375 | 625 | 78.95 |
| B `SyncCache` | 1 | 4,731,847 | 4.48–5.86 | 125 | 375 | 542 | 78.95 |
| B `SyncCache` | 2 | 2,697,451 | 1.82–3.00 | 166 | 1,750 | 13,958 | 78.97 |
| B `SyncCache` | 4 | 1,154,025 | 0.76–1.33 | 292 | 18,750 | 43,500 | 78.98 |
| B `SyncCache` | 8 | 1,706,092 | 1.30–1.84 | 292 | 16,625 | 54,792 | 78.97 |
| B `SyncCache` | 16 | 1,743,356 | 1.65–1.87 | 292 | 16,000 | 121,625 | 78.93 |
| C `ShardedCache(8)` | 1 | 5,095,958 | 3.15–5.20 | 125 | 375 | 459 | 78.96 |
| C `ShardedCache(8)` | 2 | 3,242,065 | 2.19–3.65 | 250 | 3,125 | 5,791 | 78.95 |
| C `ShardedCache(8)` | 4 | 2,869,423 | 2.72–3.53 | 375 | 6,583 | 13,584 | 78.98 |
| C `ShardedCache(8)` | 8 | 2,863,741 | 2.78–3.00 | 458 | 15,416 | 29,875 | 78.97 |
| C `ShardedCache(8)` | 16 | 2,745,281 | 2.71–2.76 | 500 | 35,042 | 64,209 | 78.92 |

### balanced

50% GET / 50% SET, 80/20 skew over 100,000 keys, capacity 40,000.

| Version | Threads | ops/sec | Range (M) | p50 ns | p95 ns | p99 ns | Hit % |
| --- | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| A `Cache` | 1 | 5,753,640 | 3.90–6.07 | 125 | 333 | 458 | 78.93 |
| B `SyncCache` | 1 | 5,472,118 | 4.09–5.64 | 125 | 333 | 417 | 78.93 |
| B `SyncCache` | 2 | 2,667,654 | 2.04–2.83 | 167 | 2,041 | 12,708 | 78.97 |
| B `SyncCache` | 4 | 1,022,205 | 0.96–1.43 | 458 | 20,291 | 48,791 | 78.93 |
| B `SyncCache` | 8 | 1,538,769 | 1.45–1.76 | 375 | 17,916 | 56,458 | 78.89 |
| B `SyncCache` | 16 | 1,691,417 | 1.18–1.83 | 333 | 20,750 | 187,500 | 78.96 |
| C `ShardedCache(8)` | 1 | 3,999,064 | 3.22–4.93 | 167 | 500 | 792 | 78.93 |
| C `ShardedCache(8)` | 2 | 2,618,546 | 2.41–2.75 | 292 | 3,625 | 7,375 | 78.95 |
| C `ShardedCache(8)` | 4 | 2,988,606 | 2.79–3.11 | 375 | 6,292 | 13,083 | 78.91 |
| C `ShardedCache(8)` | 8 | 2,894,324 | 2.84–2.94 | 459 | 15,125 | 27,666 | 78.91 |
| C `ShardedCache(8)` | 16 | 2,634,165 | 2.55–2.68 | 583 | 35,209 | 63,125 | 78.92 |

### write-heavy

10% GET / 90% SET, 80/20 skew over 100,000 keys, capacity 40,000.

| Version | Threads | ops/sec | Range (M) | p50 ns | p95 ns | p99 ns | Hit % |
| --- | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| A `Cache` | 1 | 5,023,840 | 3.36–5.63 | 125 | 334 | 500 | 79.25 |
| B `SyncCache` | 1 | 3,986,077 | 3.80–4.68 | 166 | 500 | 667 | 79.25 |
| B `SyncCache` | 2 | 2,053,278 | 1.77–2.45 | 250 | 2,875 | 17,042 | 79.13 |
| B `SyncCache` | 4 | 982,000 | 0.74–1.15 | 542 | 21,292 | 47,250 | 79.22 |
| B `SyncCache` | 8 | 1,568,616 | 1.20–1.70 | 375 | 19,500 | 60,292 | 79.27 |
| B `SyncCache` | 16 | 1,412,182 | 1.10–1.80 | 417 | 22,042 | 201,250 | 79.26 |
| C `ShardedCache(8)` | 1 | 3,629,953 | 3.08–4.83 | 167 | 500 | 875 | 79.25 |
| C `ShardedCache(8)` | 2 | 2,700,957 | 2.46–3.23 | 333 | 3,334 | 6,834 | 79.14 |
| C `ShardedCache(8)` | 4 | 2,994,311 | 2.89–3.45 | 459 | 6,000 | 12,708 | 79.22 |
| C `ShardedCache(8)` | 8 | 2,718,954 | 2.60–2.96 | 542 | 15,917 | 32,833 | 79.29 |
| C `ShardedCache(8)` | 16 | 2,590,795 | 2.25–2.62 | 667 | 35,583 | 63,666 | 79.29 |

### high-churn

20% GET / 80% SET, uniform over 200,000 keys, capacity 20,000.

| Version | Threads | ops/sec | Range (M) | p50 ns | p95 ns | p99 ns | Hit % |
| --- | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| A `Cache` | 1 | 3,097,957 | 2.95–3.43 | 250 | 500 | 708 | 9.65 |
| B `SyncCache` | 1 | 3,270,561 | 2.71–3.38 | 291 | 416 | 541 | 9.65 |
| B `SyncCache` | 2 | 1,917,162 | 1.41–1.93 | 292 | 2,750 | 19,500 | 9.84 |
| B `SyncCache` | 4 | 812,551 | 0.60–0.99 | 1,500 | 24,750 | 54,500 | 9.83 |
| B `SyncCache` | 8 | 991,603 | 0.80–1.39 | 708 | 34,417 | 96,166 | 9.79 |
| B `SyncCache` | 16 | 1,143,139 | 1.06–1.16 | 542 | 39,000 | 265,750 | 9.79 |
| C `ShardedCache(8)` | 1 | 2,703,744 | 2.23–2.89 | 333 | 500 | 709 | 9.65 |
| C `ShardedCache(8)` | 2 | 2,045,571 | 2.00–2.14 | 500 | 4,000 | 7,208 | 9.81 |
| C `ShardedCache(8)` | 4 | 2,162,067 | 2.03–2.30 | 708 | 7,667 | 15,875 | 9.80 |
| C `ShardedCache(8)` | 8 | 1,911,131 | 1.74–1.97 | 958 | 19,292 | 36,834 | 9.72 |
| C `ShardedCache(8)` | 16 | 1,720,683 | 1.66–1.77 | 1,167 | 43,000 | 77,917 | 9.75 |

### ttl-heavy

70% GET / 30% SET, every SET with a 2 s TTL, 80/20 skew over 100,000 keys,
capacity 40,000.

| Version | Threads | ops/sec | Range (M) | p50 ns | p95 ns | p99 ns | Hit % |
| --- | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| A `Cache` | 1 | 4,998,774 | 4.32–5.62 | 125 | 375 | 500 | 78.97 |
| B `SyncCache` | 1 | 4,215,533 | 3.54–5.27 | 167 | 417 | 708 | 78.97 |
| B `SyncCache` | 2 | 1,851,405 | 1.55–2.74 | 250 | 2,292 | 20,542 | 78.96 |
| B `SyncCache` | 4 | 1,095,001 | 0.72–1.33 | 375 | 19,666 | 43,333 | 78.95 |
| B `SyncCache` | 8 | 1,255,831 | 1.23–1.94 | 458 | 26,375 | 79,375 | 78.94 |
| B `SyncCache` | 16 | 1,726,937 | 1.24–1.80 | 292 | 18,542 | 157,125 | 78.92 |
| C `ShardedCache(8)` | 1 | 4,678,912 | 3.49–4.71 | 167 | 375 | 500 | 78.98 |
| C `ShardedCache(8)` | 2 | 2,985,903 | 2.48–3.39 | 250 | 3,209 | 5,959 | 78.96 |
| C `ShardedCache(8)` | 4 | 2,889,600 | 2.78–3.40 | 417 | 6,375 | 13,500 | 78.96 |
| C `ShardedCache(8)` | 8 | 2,717,483 | 2.53–2.91 | 459 | 16,000 | 31,958 | 78.95 |
| C `ShardedCache(8)` | 16 | 2,617,022 | 2.44–2.68 | 542 | 35,292 | 63,625 | 78.93 |

Every configuration in every run had zero errors.

### C against B at the same thread count

Each run prints this comparison. The cells are the median of the five per-run
values.

Throughput:

| Workload | 1 thread | 2 threads | 4 threads | 8 threads | 16 threads |
| --- | ---: | ---: | ---: | ---: | ---: |
| read-heavy | −10.4% | +20.5% | +195.6% | +67.9% | +57.5% |
| balanced | −12.6% | −2.7% | +173.3% | +85.6% | +58.6% |
| write-heavy | −8.9% | +41.8% | +221.9% | +85.9% | +59.0% |
| high-churn | −13.1% | +11.7% | +150.2% | +75.4% | +51.2% |
| ttl-heavy | −10.9% | +41.7% | +156.2% | +105.9% | +50.7% |

p99 latency (negative is better):

| Workload | 1 thread | 2 threads | 4 threads | 8 threads | 16 threads |
| --- | ---: | ---: | ---: | ---: | ---: |
| read-heavy | +9.8% | −55.1% | −72.7% | −45.5% | −46.1% |
| balanced | +26.7% | −42.0% | −71.7% | −51.1% | −66.6% |
| write-heavy | +25.0% | −64.5% | −74.6% | −51.1% | −60.8% |
| high-churn | +30.9% | −65.7% | −70.4% | −54.9% | −71.2% |
| ttl-heavy | +5.9% | −64.3% | −67.5% | −58.4% | −59.5% |

### Memory per entry

Measured as malloc bytes in use before and after filling the cache. The values
were identical in all five runs.

| Entries | Heap delta | Bytes per entry | Against the 80-byte payload |
| ---: | ---: | ---: | ---: |
| 100,000 | 20.6 MiB | 216.4 | 2.70× |
| 250,000 | 52.7 MiB | 221.2 | 2.76× |
| 500,000 | 105.5 MiB | 221.2 | 2.76× |

### `get()` against `get_into()`

Single thread, read-heavy workload.

| Variant | ops/sec | Range (M) | Average ns | p50 ns | p99 ns |
| --- | ---: | --- | ---: | ---: | ---: |
| `get()` | 5,343,209 | 3.02–5.50 | 187.2 | 125 | 459 |
| `get_into()` | 7,671,770 | 4.89–7.89 | 130.3 | 83 | 416 |

Throughput change per run: +43.6%, +62.2%, +52.2%, +35.2%, +43.4%. Median +43.6%.

---

## Single-threaded cache

`cachex_bench`, one invocation ([`cache_bench.txt`](results/cache_bench.txt)).

### Core operations

Unbounded cache, 200,000 operations per phase in shuffled key order, median of 5
repeats.

| Phase | ops/sec | Average ns |
| --- | ---: | ---: |
| SET insert | 6,921,044 | 144.5 |
| SET update | 1,283,064 | 779.4 |
| GET hit | 1,795,288 | 557.0 |
| GET miss | 18,837,048 | 53.1 |
| CONTAINS | 9,121,418 | 109.6 |
| ERASE | 2,279,817 | 438.6 |

Every phase except SET insert walks existing entries in random order, so its
cost is dominated by cache misses on scattered nodes.

### Cost of the capacity check

Uniform 50/50 GET/SET over 100,000 keys, 15 paired, order-alternated repeats.

| Configuration | ops/sec | Average ns | Evictions |
| --- | ---: | ---: | ---: |
| Unbounded | 9,118,212 | 109.7 | 0 |
| Bounded, capacity = key space | 9,082,858 | 110.1 | 0 |
| Bounded, capacity = 10% of keys | 6,896,899 | 145.0 | 80,196 |

Paired difference between the first two rows: median −0.3%, middle half −0.8% to
+0.6%, full range −20.7% to +23.1%. The interval straddles zero, so the cost is
below this harness's noise floor. The third row holds a tenth of the data, so it
is not comparable to the other two.

### Workloads

| | Workload A | Workload B |
| --- | --- | --- |
| Mix | 90% GET / 10% SET | 20% GET / 80% SET |
| Keys | 80/20 skew over 100,000 | uniform over 100,000 |
| Capacity | 40,000 | 5,000 |
| Throughput | 6,917,673 ops/sec | 4,557,396 ops/sec |
| Hit rate | 84.21% | 4.92% |
| Evictions | 31,495 | 189,939 |

### Hit rate against capacity

Workload A's request sequence at different capacities.

| Capacity | Hit rate | Evictions | ops/sec |
| --- | ---: | ---: | ---: |
| 1% (1,000) | 3.32% | 193,372 | 5,321,561 |
| 5% (5,000) | 15.92% | 168,125 | 4,097,021 |
| 10% (10,000) | 30.92% | 138,149 | 4,106,664 |
| 20% (20,000) | 57.78% | 84,385 | 4,907,996 |
| 40% (40,000) | 84.21% | 31,495 | 6,878,180 |
| 100% (100,000) | 100.00% | 0 | 10,941,019 |

Hit rates and eviction counts are deterministic and identical on every run.

### Cost of the TTL check

100,000 GET hits in shuffled order, 15 paired, order-alternated repeats.

| Case | ops/sec | Average ns | Reclaimed |
| --- | ---: | ---: | ---: |
| GET hit, no TTL | 5,058,457 | 197.7 | 0 |
| GET hit, TTL set | 4,574,208 | 218.6 | 0 |
| GET, all entries expired | 2,159,423 | 463.1 | 100,000 |

Paired difference between the first two rows: median +10.6%, middle half +8.9% to
+12.0%, full range +4.2% to +33.5%. The expired case returns nothing and deletes
each entry, so it is a different path rather than a slower version of the same
one.

---

## Over TCP

`cachex_net_bench`, one invocation ([`net_bench.txt`](results/net_bench.txt)).
Server and clients share the machine over loopback with `TCP_NODELAY`.

### Single client

20,000 requests per command, one request in flight.

| Command | req/sec | Average µs | p50 µs | p95 µs | p99 µs | Max µs |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| PING | 51,007 | 19.61 | 19.67 | 23.00 | 28.75 | 189.46 |
| SET | 47,636 | 20.99 | 20.29 | 25.04 | 32.79 | 244.42 |
| GET | 47,684 | 20.97 | 20.25 | 26.62 | 34.12 | 188.58 |

### Concurrent clients and shard count

32,000 requests per configuration, split evenly across the clients.

GET throughput (req/sec):

| Clients | 1 shard | 2 shards | 4 shards | 8 shards | Best against 1 shard |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 49,006 | 48,371 | 47,346 | 47,013 | +0.0% |
| 2 | 87,259 | 85,271 | 85,085 | 84,495 | +0.0% |
| 4 | 91,564 | 89,320 | 91,368 | 91,754 | +0.2% |
| 8 | 121,260 | 118,433 | 122,725 | 117,651 | +1.2% |
| 16 | 124,904 | 124,826 | 125,165 | 123,113 | +0.2% |

GET p99 latency (µs):

| Clients | 1 shard | 2 shards | 4 shards | 8 shards |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 29.71 | 28.04 | 37.33 | 31.21 |
| 2 | 37.38 | 36.00 | 36.33 | 37.17 |
| 4 | 57.29 | 56.08 | 56.75 | 61.29 |
| 8 | 89.42 | 132.17 | 81.12 | 93.62 |
| 16 | 149.25 | 148.12 | 147.62 | 153.88 |

SET throughput (req/sec):

| Clients | 1 shard | 2 shards | 4 shards | 8 shards | Best against 1 shard |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 46,456 | 44,795 | 47,335 | 47,637 | +2.5% |
| 2 | 85,779 | 81,117 | 80,775 | 57,546 | +0.0% |
| 4 | 92,419 | 91,010 | 91,726 | 94,159 | +1.9% |
| 8 | 118,183 | 112,007 | 120,539 | 115,919 | +2.0% |
| 16 | 111,804 | 113,403 | 121,561 | 100,762 | +8.7% |

The SET run was noisier than the GET run. The low 8-shard cells at 2 and 16
clients do not follow any trend in shard count.

### In-process, no sockets

N threads calling `ShardedCache::get()` directly, 600,000 calls per cell (ops/sec).

| Threads | 1 shard | 2 shards | 4 shards | 8 shards | Best against 1 shard |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 6,277,614 | 6,285,983 | 6,535,049 | 6,351,340 | +4.1% |
| 2 | 2,855,990 | 3,327,728 | 3,163,020 | 4,030,134 | +41.1% |
| 4 | 1,666,489 | 2,689,457 | 3,390,673 | 4,584,879 | +175.1% |
| 8 | 2,977,744 | 1,961,763 | 2,887,438 | 4,193,375 | +40.8% |
| 16 | 2,933,021 | 2,040,351 | 2,656,544 | 4,105,306 | +40.0% |

### Global LRU against per-shard LRU

Skewed 80/20 workload, cache-aside, single thread, so only the eviction policy
differs.

| Capacity | 1 shard | 2 shards | 4 shards | 8 shards | Worst cost |
| --- | ---: | ---: | ---: | ---: | ---: |
| 5% (5,000) | 15.92% | 15.91% | 15.93% | 15.90% | −0.01 pp |
| 10% (10,000) | 30.92% | 30.92% | 30.91% | 30.93% | −0.02 pp |
| 20% (20,000) | 57.78% | 57.79% | 57.81% | 57.78% | +0.00 pp |
| 40% (40,000) | 84.21% | 84.21% | 84.21% | 84.20% | −0.01 pp |

---

## Persistence

`cachex_persist_bench`, one invocation
([`persist_bench.txt`](results/persist_bench.txt)). 16-byte keys, 64-byte values,
8 shards, median of 5 repeats, snapshot in `/tmp`.

Entries without a TTL:

| Entries | Save ms | Load ms | Save µs per entry | Snapshot KiB | Bytes per entry |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1,000 | 0.97 | 0.50 | 0.968 | 87.9 | 90.0 |
| 10,000 | 4.32 | 4.56 | 0.432 | 878.9 | 90.0 |
| 100,000 | 51.25 | 46.40 | 0.513 | 8,789.1 | 90.0 |
| 500,000 | 281.53 | 259.24 | 0.563 | 43,945.4 | 90.0 |

Entries with a TTL:

| Entries | Save ms | Load ms | Save µs per entry | Snapshot KiB | Bytes per entry |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1,000 | 0.61 | 0.58 | 0.612 | 92.8 | 95.0 |
| 10,000 | 4.58 | 5.51 | 0.458 | 927.8 | 95.0 |
| 100,000 | 49.63 | 55.94 | 0.496 | 9,277.4 | 95.0 |
| 500,000 | 261.85 | 300.32 | 0.524 | 46,386.8 | 95.0 |

The snapshot is 90 bytes per entry, 1.13 times the 80-byte payload. The 10 extra
bytes are the record header and trailing newline. A TTL adds 5 bytes for the
millisecond count. Loads read a file that was just written, so they are served
from the page cache.
