# Benchmarks

Empty for now — there is no cache engine to measure yet.

Enable with:

```bash
cmake -S . -B build -DCACHEX_BUILD_BENCHMARKS=ON
```

Planned once the core cache exists:

- throughput (operations/second) for `GET` / `SET` under a fixed key distribution
- latency percentiles (p50 / p99), which matter far more than the average for a cache
- hit rate under different eviction policies and working-set sizes
- scaling across threads, to show where lock contention starts to bite

Measurement will use `std::chrono::steady_clock` — it is monotonic, so an NTP
adjustment mid-run cannot produce a negative duration the way `system_clock` can.
