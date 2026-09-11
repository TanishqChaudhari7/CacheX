# Benchmark Results

> **These numbers are specific to one machine and mean nothing in isolation.**
> They exist as a baseline to compare *future versions of CacheX against*, on the
> same hardware, in the same sitting. Do not compare them to numbers from another
> machine, and do not compare them to Redis — Redis pays network and protocol
> costs this in-process benchmark does not.

Methodology: [ARCHITECTURE.md §7](../ARCHITECTURE.md#7-benchmark-methodology).

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DCACHEX_BUILD_BENCHMARKS=ON
cmake --build build/release -j
./build/release/bin/cachex_bench
```

---

## Stage 2 baseline — core cache, single-threaded, unbounded

| | |
| --- | --- |
| Date | 2026-09-11 |
| Machine | Apple M2 Pro, 12 cores, 16 GB RAM |
| OS | macOS 26.5.2 (arm64) |
| Compiler | AppleClang 21.0.0, `-O3 -DNDEBUG`, C++17 |
| CacheX version | 0.1.0 |
| Sample | 12 invocations of the binary, each itself reporting the median of 5 internal repeats |

### Headline numbers

Median across all 12 invocations, with the full observed range:

| phase | median ops/sec | median latency | observed latency range |
| --- | ---: | ---: | --- |
| SET insert | 6,690,725 | 150 ns | 133 – 186 ns |
| SET update | 1,877,021 | 533 ns | 361 – 763 ns |
| GET hit | 3,021,391 | 331 ns | 234 – 477 ns |
| GET miss | 19,833,811 | 50 ns | 40 – 54 ns |
| CONTAINS | 17,874,566 | 56 ns | 41 – 66 ns |
| ERASE | 3,742,265 | 267 ns | 181 – 383 ns |

Raw output of one representative invocation:

```
CacheX 0.1.0 benchmark
=================================

build        : Release
compiler     : AppleClang 21.0.0.21000101
operations   : 200000 per phase
key size     : 16 bytes (fits small-string storage)
value size   : 64 bytes (heap allocated)
repeats      : 5, median reported
seed         : 42
clock        : std::chrono::steady_clock

phase                ops  total (ms)       ops/sec    avg (ns)
--------------------------------------------------------------
SET insert        200000       28.11       7114073       140.6
SET update        200000       80.69       2478706       403.4
GET hit           200000       54.69       3657112       273.4
GET miss          200000       10.18      19645883        50.9
CONTAINS          200000       11.38      17568003        56.9
ERASE             200000       44.97       4447714       224.8

checksum: 79200000  (printed only so the optimiser cannot discard the work)
```

---

## ⚠️ The variance is much larger than it looks — read this before comparing anything

Three phases — `SET update`, `GET hit`, and `ERASE` — do not vary smoothly. Across
12 invocations of the *same binary* on the *same machine*, they fell into two
distinct clusters roughly **2× apart**, with almost nothing in between:

| phase | fast cluster | slow cluster | ratio |
| --- | ---: | ---: | ---: |
| SET update | ~2.2 – 2.8 M ops/s | ~1.3 – 1.6 M ops/s | 1.9× |
| GET hit | ~3.7 – 4.3 M ops/s | ~2.1 – 2.4 M ops/s | 1.8× |
| ERASE | ~4.4 – 5.5 M ops/s | ~2.6 – 3.0 M ops/s | 1.8× |

The other three phases (`SET insert`, `GET miss`, `CONTAINS`) stayed within about
±20% throughout and showed no clustering.

**The cause was not established.** What is known:

- The binary was byte-identical across all runs, and the printed `checksum` was identical every time — the *work* is deterministic, only the *timing* is not.
- The slow cluster was first seen while Spotlight (`mds_stores`) was at ~96% CPU. But the slow cluster persisted after Spotlight dropped to 0%, and the fast cluster later returned while a WebKit process was at 100% CPU. **Simple CPU contention does not explain it.**
- The three affected phases are exactly the three that chase pointers through 200,000 scattered list nodes in shuffled order. The three unaffected phases are the ones that do a single hash lookup and stop.

The most plausible explanation — offered as a **hypothesis, not a finding** — is
performance-core vs. efficiency-core scheduling. Apple Silicon migrates processes
between core types based on system state, and E-cores have smaller caches and less
memory bandwidth. That would hit memory-latency-bound work hard and hash-lookup
work barely, and it would produce two clusters rather than a smooth spread —
which is what was observed. Confirming it needs per-core instrumentation that is
out of scope here.

### What this means in practice

1. **Absolute numbers from different sittings are not comparable.** On this machine a 2× difference can appear with no code change at all.
2. **Ratios within a single run are stable and are the trustworthy signal.** Every relationship in the next section held in *both* clusters.
3. **To compare two versions of CacheX, run both back to back, interleaved, in the same sitting** — and treat anything under ~2× on the memory-bound phases as unproven.
4. This is precisely why Stage 5 needs a better harness. A laptop under an unknown scheduler is not a measurement instrument.

---

## Reading the results

Four observations. All are consequences of the design, and all held in both
performance clusters — which is why they are worth trusting when the absolute
numbers are not.

1. **`CONTAINS` ≈ `GET miss`** (56 ns vs. 50 ns at the median). Both do one hash
   and one bucket probe, then stop. This is the cost of the `unordered_map`
   lookup on its own, and it is the cheapest thing the cache can do.

2. **`GET hit` costs ~6× `CONTAINS`** (331 ns vs. 56 ns), despite performing the
   identical lookup. The difference is the two things a hit does and a
   `contains` does not: splice the node to the front of the list, and copy the
   64-byte value into the returned `std::optional<std::string>` — which
   heap-allocates, because 64 bytes exceeds small-string capacity. This is the
   measured price of
   [returning a copy rather than a reference](../ARCHITECTURE.md#6-ownership-and-lifetime),
   and it is the first thing to attack in Stage 11.

3. **`SET update` costs ~3.5× `SET insert`** (533 ns vs. 150 ns) while doing
   strictly *less* work — no allocation, no map insertion, no new node. The cause
   is the access pattern, not the operation: inserts walk keys in order and touch
   freshly allocated, contiguous memory, while updates walk a shuffled order and
   chase pointers across 200,000 scattered nodes. Nearly every update is a cache
   miss. This is the clearest demonstration in the project that **algorithmic
   complexity is not the whole story** — both paths are O(1).

4. **Nothing here contradicts O(1).** The phases span ~10× in cost, but every bit
   of that is constant factors: allocation, copying, and memory locality. None of
   it grows with the number of keys.

---

## Known limitations of this baseline

- **Average latency only.** Timing each individual operation would cost a `steady_clock` read (~20–25 ns) per measurement — comparable to the operations themselves — and would distort what it measures. Percentiles need a different technique and arrive in Stage 5.
- **The measurement environment is not controlled.** See the variance section above. This is the single biggest weakness of the Stage 2 harness.
- **One thread, one key size, one value size, uniform access.** Real workloads are skewed — a small set of hot keys serving most traffic. Skewed distributions arrive with the LRU work in Stage 3, where hit rate starts to mean something.
- **No hit-rate measurement.** There is nothing to measure yet: the cache is unbounded, so nothing is ever evicted and a key is present iff it was set.
- **In-process.** No sockets, no protocol parsing, no syscalls. Expect these per-operation costs to be dwarfed by network cost once Stage 6 lands.
