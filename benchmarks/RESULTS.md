# Benchmark Results

> **These numbers are specific to one machine and mean nothing in isolation.**
> They exist as a baseline to compare *future versions of CacheX against*, on the
> same hardware, in the same sitting. Do not compare them to numbers from another
> machine, and do not compare them to Redis — Redis pays network and protocol
> costs this in-process benchmark does not.

Methodology: [ARCHITECTURE.md §10](../ARCHITECTURE.md#10-benchmark-methodology).

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DCACHEX_BUILD_BENCHMARKS=ON
cmake --build build/release -j
./build/release/bin/cachex_bench
```

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
- **Memory per entry is not measured.** ARCHITECTURE §6 gives an estimate from the data layout; that is not the same as a measurement. ⬜ Stage 11.
- **Two synthetic distributions.** 80/20 skew and uniform. Real traffic is Zipfian with a time-varying hot set, and neither of these captures a working set that shifts.
- **In-process.** No sockets, no protocol parsing, no syscalls. The Stage 5 section above measures the same cache over TCP, where these per-operation costs turn out to be 2-3% of a request.
