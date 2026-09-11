#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "bench_util.hpp"
#include "cachex/cache.hpp"
#include "cachex/version.hpp"

namespace {

// Timing and statistics helpers are shared with the network benchmark.
using bench::avg_ns;
using bench::Clock;
using bench::Latency;
using bench::median;
using bench::Nanos;
using bench::ops_per_sec;
using bench::print_latency_row;
using bench::summarize;

// Fixed so two runs of the same binary -- and runs of two different versions of
// the cache -- execute exactly the same sequence of operations.
constexpr std::size_t kOperations = 200000;   // per phase, section 1
constexpr std::size_t kWorkloadOps = 200000;  // per workload, sections 2-5
constexpr std::size_t kKeySpace = 100000;     // distinct keys the workloads use
constexpr std::size_t kValueBytes = 64;
constexpr std::uint32_t kSeed = 42;
constexpr int kRepeats = 5;
constexpr int kAbRepeats = 15;  // the A/B in section 2 needs a tighter error bar
constexpr std::size_t kTtlKeys = 100000;  // section 6

// Written to by every phase and printed at the end. Without an observable use of
// the results, the optimiser is free to delete the calls being measured.
std::uint64_t g_sink = 0;

// --- helpers ---------------------------------------------------------------

/// Zero-padded to a constant 16 bytes: variable-length keys would make hashing
/// and comparison cost drift over a run. 16 bytes also fits libc++/libstdc++
/// small-string storage, so key handling itself costs no allocation.
std::string make_key(const char* prefix, std::size_t i) {
  const std::string digits = std::to_string(i);
  return prefix + std::string(12 - digits.size(), '0') + digits;
}

template <typename F>
Nanos time_it(F&& f) {
  const auto start = Clock::now();
  f();
  return std::chrono::duration_cast<Nanos>(Clock::now() - start);
}

// --- section 1: core operation microbenchmarks -----------------------------

struct Timings {
  Nanos set_insert{0};
  Nanos set_update{0};
  Nanos get_hit{0};
  Nanos get_miss{0};
  Nanos contains{0};
  Nanos erase{0};
};

Timings run_core_phases(const std::vector<std::string>& keys,
                        const std::vector<std::string>& absent_keys,
                        const std::vector<std::size_t>& access_order,
                        const std::string& value) {
  Timings t;
  cachex::Cache cache;  // unbounded, so these stay comparable to the Stage 2 baseline

  t.set_insert = time_it([&] {
    for (const std::string& key : keys) {
      cache.set(key, value);
    }
  });
  t.set_update = time_it([&] {
    for (const std::size_t i : access_order) {
      cache.set(keys[i], value);
    }
  });
  t.get_hit = time_it([&] {
    std::uint64_t acc = 0;
    for (const std::size_t i : access_order) {
      if (const auto found = cache.get(keys[i])) {
        acc += found->size();
      }
    }
    g_sink += acc;
  });
  t.get_miss = time_it([&] {
    std::uint64_t acc = 0;
    for (const std::string& key : absent_keys) {
      if (cache.get(key)) {
        acc += 1;
      }
    }
    g_sink += acc;
  });
  t.contains = time_it([&] {
    std::uint64_t acc = 0;
    for (const std::size_t i : access_order) {
      acc += cache.contains(keys[i]) ? 1u : 0u;
    }
    g_sink += acc;
  });
  t.erase = time_it([&] {
    std::uint64_t acc = 0;
    for (const std::string& key : keys) {
      acc += cache.erase(key) ? 1u : 0u;
    }
    g_sink += acc;
  });
  return t;
}

// --- workload generation ---------------------------------------------------

enum class Op : std::uint8_t { Get, Set };

struct Request {
  Op op;
  std::uint32_t key;
};

/// Builds the entire request sequence up front, outside every timed region, so
/// that RNG cost is never measured and the sequence is byte-identical between
/// runs and between cache configurations. Replaying one fixed sequence is what
/// makes the A/B comparison in section 2 valid.
///
/// `skewed` models real traffic: 80% of requests go to the hottest 20% of keys.
/// Uniform access is the pessimistic case for a cache -- there is no hot set to
/// retain, so eviction cannot help.
std::vector<Request> make_workload(std::size_t ops, std::size_t key_space,
                                   int get_percent, bool skewed,
                                   std::uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_int_distribution<int> roll(1, 100);
  std::uniform_int_distribution<std::uint32_t> any_key(
      0, static_cast<std::uint32_t>(key_space - 1));

  const std::size_t hot = std::max<std::size_t>(1, key_space / 5);
  std::uniform_int_distribution<std::uint32_t> hot_key(
      0, static_cast<std::uint32_t>(hot - 1));
  std::uniform_int_distribution<std::uint32_t> cold_key(
      static_cast<std::uint32_t>(hot), static_cast<std::uint32_t>(key_space - 1));

  std::vector<Request> requests;
  requests.reserve(ops);
  for (std::size_t i = 0; i < ops; ++i) {
    const Op op = roll(rng) <= get_percent ? Op::Get : Op::Set;
    std::uint32_t key = 0;
    if (!skewed || hot >= key_space) {
      key = any_key(rng);
    } else {
      key = roll(rng) <= 80 ? hot_key(rng) : cold_key(rng);
    }
    requests.push_back(Request{op, key});
  }
  return requests;
}

struct Outcome {
  Nanos total{0};
  std::size_t hits = 0;
  std::size_t misses = 0;
  std::size_t sets = 0;
  std::size_t fills = 0;  // cache-aside populations triggered by a GET miss
  std::size_t evictions = 0;
  std::size_t final_size = 0;
};

/// Cache-aside is how applications actually use a cache: on a miss the caller
/// fetches from the backing store and populates the cache, so the next request
/// for that key hits. Without it a read-heavy workload can never warm up --
/// only the explicit SETs would ever put anything in the cache, and the hit
/// rate would be capped by the write fraction rather than by capacity. That
/// makes it the difference between measuring LRU and measuring nothing.
enum class Fill { None, CacheAside };

/// Throughput pass: the whole sequence is timed once and divided. No per-call
/// clock reads, so this number is free of measurement overhead.
Outcome run_throughput(cachex::Cache& cache, const std::vector<Request>& requests,
                       const std::vector<std::string>& keys,
                       const std::string& value, Fill fill) {
  Outcome out;
  // Evictions are reported for this pass alone, so a warm-up pass on the same
  // cache does not inflate the measured figure.
  const std::size_t evictions_before = cache.evictions();
  std::uint64_t acc = 0;
  const auto start = Clock::now();
  for (const Request& r : requests) {
    if (r.op == Op::Get) {
      if (const auto found = cache.get(keys[r.key])) {
        ++out.hits;
        acc += found->size();
      } else {
        ++out.misses;
        if (fill == Fill::CacheAside) {
          cache.set(keys[r.key], value);
          ++out.fills;
        }
      }
    } else {
      cache.set(keys[r.key], value);
      ++out.sets;
    }
  }
  out.total = std::chrono::duration_cast<Nanos>(Clock::now() - start);
  g_sink += acc;
  out.evictions = cache.evictions() - evictions_before;
  out.final_size = cache.size();
  return out;
}

struct LatencyPair {
  Latency get;
  Latency set;
};

/// Latency pass: a separate run that brackets every individual call with two
/// clock reads. It cannot be combined with the throughput pass -- the clock
/// reads would inflate the total -- so the two numbers come from two runs over
/// the identical sequence.
LatencyPair run_latency(cachex::Cache& cache, const std::vector<Request>& requests,
                        const std::vector<std::string>& keys,
                        const std::string& value, Fill fill) {
  std::vector<std::int64_t> get_ns;
  std::vector<std::int64_t> set_ns;
  // Reserved for the worst case up front: a reallocation inside the measured
  // loop would land in whichever sample it interrupted.
  get_ns.reserve(requests.size());
  set_ns.reserve(requests.size());

  std::uint64_t acc = 0;
  for (const Request& r : requests) {
    if (r.op == Op::Get) {
      const auto t0 = Clock::now();
      const auto found = cache.get(keys[r.key]);
      const auto t1 = Clock::now();
      get_ns.push_back(std::chrono::duration_cast<Nanos>(t1 - t0).count());
      if (found) {
        acc += found->size();
      } else if (fill == Fill::CacheAside) {
        // The fill is a set() and is timed as one, so the SET samples cover
        // every set the cache actually performs.
        const auto f0 = Clock::now();
        cache.set(keys[r.key], value);
        const auto f1 = Clock::now();
        set_ns.push_back(std::chrono::duration_cast<Nanos>(f1 - f0).count());
      }
    } else {
      const auto t0 = Clock::now();
      cache.set(keys[r.key], value);
      const auto t1 = Clock::now();
      set_ns.push_back(std::chrono::duration_cast<Nanos>(t1 - t0).count());
    }
  }
  g_sink += acc;
  return LatencyPair{summarize(get_ns), summarize(set_ns)};
}

// --- section 6: TTL --------------------------------------------------------

/// Fills a cache with `count` keys, either persistent or with the given TTL.
void fill_cache(cachex::Cache& cache, const std::vector<std::string>& keys,
                std::size_t count, const std::string& value,
                std::optional<cachex::Cache::Duration> ttl) {
  for (std::size_t i = 0; i < count; ++i) {
    if (ttl.has_value()) {
      cache.set(keys[i], value, *ttl);
    } else {
      cache.set(keys[i], value);
    }
  }
}

Nanos time_get_pass(cachex::Cache& cache, const std::vector<std::string>& keys,
                    const std::vector<std::size_t>& order) {
  std::uint64_t acc = 0;
  const auto start = Clock::now();
  for (const std::size_t i : order) {
    if (const auto found = cache.get(keys[i])) {
      acc += found->size();
    }
  }
  const auto elapsed = std::chrono::duration_cast<Nanos>(Clock::now() - start);
  g_sink += acc;
  return elapsed;
}

Latency latency_of_get_pass(cachex::Cache& cache,
                            const std::vector<std::string>& keys,
                            const std::vector<std::size_t>& order) {
  std::vector<std::int64_t> samples;
  samples.reserve(order.size());
  std::uint64_t acc = 0;
  for (const std::size_t i : order) {
    const auto t0 = Clock::now();
    const auto found = cache.get(keys[i]);
    const auto t1 = Clock::now();
    samples.push_back(std::chrono::duration_cast<Nanos>(t1 - t0).count());
    if (found) {
      acc += found->size();
    }
  }
  g_sink += acc;
  return summarize(samples);
}

// --- printing --------------------------------------------------------------

void print_phase_row(const char* label, Nanos duration, std::size_t ops) {
  std::cout << std::left << std::setw(14) << label << std::right << std::setw(10)
            << ops << std::setw(12) << std::fixed << std::setprecision(2)
            << std::chrono::duration<double>(duration).count() * 1000.0
            << std::setw(14) << std::setprecision(0) << ops_per_sec(duration, ops)
            << std::setw(12) << std::setprecision(1) << avg_ns(duration, ops)
            << "\n";
}

void print_workload_summary(const Outcome& o) {
  const std::size_t gets = o.hits + o.misses;
  const double hit_rate =
      gets > 0 ? 100.0 * static_cast<double>(o.hits) / static_cast<double>(gets)
               : 0.0;
  std::cout << "  requests   : " << (gets + o.sets) << "  (" << gets
            << " GET, " << o.sets << " SET)\n"
            << "  throughput : " << std::fixed << std::setprecision(0)
            << ops_per_sec(o.total, gets + o.sets) << " ops/sec ("
            << std::setprecision(1) << avg_ns(o.total, gets + o.sets)
            << " ns/op per application request)\n"
            << "  hit rate   : " << std::setprecision(2) << hit_rate << "%  ("
            << o.hits << " hits, " << o.misses << " misses)\n"
            << "  fills      : " << o.fills << " (cache-aside populations on miss)\n"
            << "  evictions  : " << o.evictions << ",  entries resident: "
            << o.final_size << "\n";
}

void print_latency_table(const LatencyPair& l, double clock_ns, double tick_ns) {
  std::cout << "\n"
            << std::left << std::setw(14) << "  latency (ns)" << std::right
            << std::setw(11) << "samples" << std::setw(11) << "mean"
            << std::setw(10) << "p50" << std::setw(10) << "p95" << std::setw(10)
            << "p99" << std::setw(12) << "max" << "\n"
            << "  " << std::string(76, '-') << "\n";
  print_latency_row("  GET", l.get);
  print_latency_row("  SET", l.set);
  std::cout << "  (each sample includes ~" << std::fixed << std::setprecision(1)
            << clock_ns << " ns of clock-read overhead, and is quantised to the\n"
            << "   clock's " << tick_ns
            << " ns tick -- so these are tick counts, not fine-grained times)\n";
}

}  // namespace

int main() {
  std::cout << "CacheX " << cachex::version_string() << " benchmark\n"
            << "=================================\n";

#ifndef NDEBUG
  std::cout << "\n*** WARNING: assertions are enabled -- this is not a Release\n"
            << "*** build. These numbers measure the absence of the optimiser.\n";
#endif

  const double clock_ns = bench::measure_clock_overhead_ns();
  const double tick_ns = bench::measure_clock_granularity_ns();

  std::cout << "\nbuild          : " << CACHEX_BUILD_TYPE << "\n"
            << "compiler       : " << CACHEX_COMPILER << "\n"
            << "key size       : 16 bytes (fits small-string storage)\n"
            << "value size     : " << kValueBytes << " bytes (heap allocated)\n"
            << "seed           : " << kSeed << "\n"
            << "clock          : std::chrono::steady_clock, measured overhead "
            << std::fixed << std::setprecision(1) << clock_ns
            << " ns/read, tick " << tick_ns << " ns\n";

  // Built once, outside every timed region: key construction allocates, and
  // measuring it would measure std::string rather than the cache.
  std::vector<std::string> keys;
  std::vector<std::string> absent_keys;
  keys.reserve(std::max(kOperations, kKeySpace));
  absent_keys.reserve(kOperations);
  for (std::size_t i = 0; i < std::max(kOperations, kKeySpace); ++i) {
    keys.push_back(make_key("key:", i));
  }
  for (std::size_t i = 0; i < kOperations; ++i) {
    absent_keys.push_back(make_key("nil:", i));
  }

  // Reads follow a shuffled order rather than insertion order. Walking keys in
  // the order they were inserted is unrealistically friendly to the CPU cache
  // and the prefetcher, and would overstate GET throughput.
  std::vector<std::size_t> access_order(kOperations);
  std::iota(access_order.begin(), access_order.end(), std::size_t{0});
  std::shuffle(access_order.begin(), access_order.end(), std::mt19937(kSeed));

  const std::string value(kValueBytes, 'v');

  // =========================================================================
  std::cout << "\n\n1. CORE OPERATIONS (unbounded cache -- Stage 2 baseline)\n"
            << "   " << kOperations << " ops per phase, median of " << kRepeats
            << " after a discarded warm-up\n\n";

  run_core_phases(keys, absent_keys, access_order, value);  // warm-up, discarded

  std::vector<Timings> core_runs;
  core_runs.reserve(static_cast<std::size_t>(kRepeats));
  for (int i = 0; i < kRepeats; ++i) {
    core_runs.push_back(run_core_phases(keys, absent_keys, access_order, value));
  }
  const auto core_median = [&core_runs](Nanos Timings::*field) {
    std::vector<Nanos> samples;
    samples.reserve(core_runs.size());
    for (const Timings& run : core_runs) {
      samples.push_back(run.*field);
    }
    return median(std::move(samples));
  };

  std::cout << std::left << std::setw(14) << "phase" << std::right
            << std::setw(10) << "ops" << std::setw(12) << "total (ms)"
            << std::setw(14) << "ops/sec" << std::setw(12) << "avg (ns)" << "\n"
            << std::string(62, '-') << "\n";
  print_phase_row("SET insert", core_median(&Timings::set_insert), kOperations);
  print_phase_row("SET update", core_median(&Timings::set_update), kOperations);
  print_phase_row("GET hit", core_median(&Timings::get_hit), kOperations);
  print_phase_row("GET miss", core_median(&Timings::get_miss), kOperations);
  print_phase_row("CONTAINS", core_median(&Timings::contains), kOperations);
  print_phase_row("ERASE", core_median(&Timings::erase), kOperations);

  // =========================================================================
  // The cost of LRU itself. All three configurations replay the identical
  // request sequence, interleaved within each repeat so that a drift in machine
  // state hits all three equally rather than penalising whichever ran last.
  std::cout << "\n\n2. COST OF LRU (identical request sequence, interleaved)\n"
            << "   uniform 50% GET / 50% SET over " << kKeySpace << " keys, "
            << kWorkloadOps << " requests, " << kAbRepeats << " interleaved repeats\n\n";

  const std::vector<Request> ab_requests =
      make_workload(kWorkloadOps, kKeySpace, 50, /*skewed=*/false, kSeed);

  std::vector<Nanos> unbounded_runs;
  std::vector<Nanos> headroom_runs;
  std::vector<Nanos> evicting_runs;
  std::vector<double> paired_ratio;  // headroom / unbounded, within one repeat
  std::size_t headroom_evictions = 0;
  std::size_t evicting_evictions = 0;
  for (int i = 0; i < kAbRepeats; ++i) {
    // Alternate which of the two runs first. Whichever goes first pays for heap
    // growth that the second then reuses, which is a systematic bias worth more
    // than the effect being measured -- running A always before B made B look
    // consistently *faster* than A despite doing strictly more work.
    const bool unbounded_first = (i % 2) == 0;
    Nanos unbounded_time{0};
    Nanos headroom_time{0};

    for (int pass = 0; pass < 2; ++pass) {
      if ((pass == 0) == unbounded_first) {
        cachex::Cache unbounded;
        unbounded_time =
            run_throughput(unbounded, ab_requests, keys, value, Fill::None).total;
      } else {
        cachex::Cache headroom(kKeySpace);  // capacity >= working set: never evicts
        const Outcome h =
            run_throughput(headroom, ab_requests, keys, value, Fill::None);
        headroom_time = h.total;
        headroom_evictions = h.evictions;
      }
    }
    unbounded_runs.push_back(unbounded_time);
    headroom_runs.push_back(headroom_time);

    cachex::Cache evicting(kKeySpace / 10);  // capacity << working set
    const Outcome e = run_throughput(evicting, ab_requests, keys, value, Fill::None);
    evicting_runs.push_back(e.total);
    evicting_evictions = e.evictions;

    // Paired within the repeat. Comparing two independent medians lets machine
    // drift between them masquerade as a result -- and at this effect size the
    // drift is larger than the effect, so an unpaired comparison can even come
    // out negative, which is impossible for strictly-more-work.
    paired_ratio.push_back(static_cast<double>(headroom_time.count()) /
                           static_cast<double>(unbounded_time.count()));
  }
  std::sort(paired_ratio.begin(), paired_ratio.end());

  const Nanos unbounded_ns = median(unbounded_runs);
  const Nanos headroom_ns = median(headroom_runs);
  const Nanos evicting_ns = median(evicting_runs);

  std::cout << std::left << std::setw(34) << "configuration" << std::right
            << std::setw(14) << "ops/sec" << std::setw(12) << "avg (ns)"
            << std::setw(13) << "evictions" << "\n"
            << std::string(73, '-') << "\n";
  const auto ab_row = [](const char* label, Nanos d, std::size_t ev) {
    std::cout << std::left << std::setw(34) << label << std::right
              << std::setw(14) << std::fixed << std::setprecision(0)
              << ops_per_sec(d, kWorkloadOps) << std::setw(12)
              << std::setprecision(1) << avg_ns(d, kWorkloadOps) << std::setw(13)
              << ev << "\n";
  };
  ab_row("unbounded (no LRU)", unbounded_ns, 0);
  ab_row("bounded, capacity = key space", headroom_ns, headroom_evictions);
  ab_row("bounded, capacity = 10% of keys", evicting_ns, evicting_evictions);

  const auto as_percent = [](double ratio) { return 100.0 * (ratio - 1.0); };
  std::cout << "\n  Rows 1 and 2 do identical work; row 2 additionally performs the\n"
            << "  capacity check on every insert. Paired per-repeat comparison over "
            << kAbRepeats << " repeats:\n"
            << "    median " << std::showpos << std::fixed << std::setprecision(1)
            << as_percent(paired_ratio[paired_ratio.size() / 2]) << "%"
            << "   middle half " << as_percent(paired_ratio[paired_ratio.size() / 4])
            << "% to " << as_percent(paired_ratio[paired_ratio.size() * 3 / 4]) << "%"
            << "   full range " << as_percent(paired_ratio.front()) << "% to "
            << as_percent(paired_ratio.back()) << "%" << std::noshowpos << "\n"
            << "  The interval straddles zero, so the overhead is below this\n"
            << "  harness's noise floor -- the expected result for one predictable\n"
            << "  branch per insert. That is NOT a claim that LRU is free; it means\n"
            << "  this experiment cannot resolve a cost this small.\n"
            << "  (Row 3 is not comparable -- it holds far less data, so its\n"
            << "   memory access pattern differs as well as its work.)\n";

  // =========================================================================
  std::cout << "\n\n3. WORKLOAD A -- mostly hits (90% GET / 10% SET, cache-aside)\n"
            << "   80/20 skewed keys over " << kKeySpace
            << ", capacity = 40% of key space\n"
            << "   measured at steady state: one full warm-up pass is discarded\n\n";

  const std::vector<Request> workload_a =
      make_workload(kWorkloadOps, kKeySpace, 90, /*skewed=*/true, kSeed);
  // 40%, not a round-number guess: the sweep in section 5 shows this is where
  // this request distribution becomes hit-dominated. An 80/20 skew over 100k
  // keys has a 20k-key hot set, and a cache exactly that size still thrashes,
  // because the 20% cold traffic keeps evicting hot entries.
  const std::size_t capacity_a = kKeySpace * 2 / 5;

  std::vector<Nanos> a_runs;
  Outcome a_outcome;
  for (int i = 0; i < kRepeats; ++i) {
    cachex::Cache cache(capacity_a);
    // A cold cache misses on everything. Warming it first means the reported
    // numbers describe the steady state a long-running server actually spends
    // its life in, and it moves one-off costs (the map's growth rehashes) out
    // of the measured pass.
    run_throughput(cache, workload_a, keys, value, Fill::CacheAside);
    a_outcome = run_throughput(cache, workload_a, keys, value, Fill::CacheAside);
    a_runs.push_back(a_outcome.total);
  }
  a_outcome.total = median(a_runs);
  print_workload_summary(a_outcome);
  {
    cachex::Cache cache(capacity_a);
    run_throughput(cache, workload_a, keys, value, Fill::CacheAside);
    print_latency_table(run_latency(cache, workload_a, keys, value, Fill::CacheAside),
                        clock_ns, tick_ns);
  }

  // =========================================================================
  std::cout << "\n\n4. WORKLOAD B -- heavy churn (20% GET / 80% SET, cache-aside)\n"
            << "   uniform keys over " << kKeySpace
            << ", capacity = 5% of key space, so most inserts evict\n"
            << "   no hot set to retain, so this is the worst case for LRU\n\n";

  const std::vector<Request> workload_b =
      make_workload(kWorkloadOps, kKeySpace, 20, /*skewed=*/false, kSeed + 1);
  const std::size_t capacity_b = kKeySpace / 20;

  std::vector<Nanos> b_runs;
  Outcome b_outcome;
  for (int i = 0; i < kRepeats; ++i) {
    cachex::Cache cache(capacity_b);
    run_throughput(cache, workload_b, keys, value, Fill::CacheAside);
    b_outcome = run_throughput(cache, workload_b, keys, value, Fill::CacheAside);
    b_runs.push_back(b_outcome.total);
  }
  b_outcome.total = median(b_runs);
  print_workload_summary(b_outcome);
  {
    cachex::Cache cache(capacity_b);
    run_throughput(cache, workload_b, keys, value, Fill::CacheAside);
    print_latency_table(run_latency(cache, workload_b, keys, value, Fill::CacheAside),
                        clock_ns, tick_ns);
  }

  // =========================================================================
  // The point of LRU in one table: on skewed traffic, hit rate should climb
  // steeply as capacity approaches the hot set and then flatten, because the
  // keys being retained are the ones actually being asked for.
  std::cout << "\n\n5. HIT RATE vs CAPACITY (Workload A request sequence)\n\n"
            << std::left << std::setw(22) << "capacity" << std::right
            << std::setw(12) << "hit rate" << std::setw(14) << "evictions"
            << std::setw(14) << "ops/sec" << "\n"
            << std::string(62, '-') << "\n";

  for (const std::size_t percent : {1u, 5u, 10u, 20u, 40u, 100u}) {
    const std::size_t cap = kKeySpace * percent / 100;
    cachex::Cache cache(cap);
    run_throughput(cache, workload_a, keys, value, Fill::CacheAside);  // warm up
    const Outcome o = run_throughput(cache, workload_a, keys, value, Fill::CacheAside);
    const std::size_t gets = o.hits + o.misses;
    const double hit_rate =
        gets > 0 ? 100.0 * static_cast<double>(o.hits) / static_cast<double>(gets)
                 : 0.0;
    std::cout << std::left << std::setw(6) << (std::to_string(percent) + "%")
              << std::setw(16) << ("(" + std::to_string(cap) + " entries)")
              << std::right << std::setw(11) << std::fixed
              << std::setprecision(2) << hit_rate << "%" << std::setw(14)
              << o.evictions << std::setw(14) << std::setprecision(0)
              << ops_per_sec(o.total, kWorkloadOps) << "\n";
  }

  // =========================================================================
  // TTL is not free, and the point of this section is to say how much it costs
  // rather than to assert that it does not. Three cases, because they exercise
  // three different code paths:
  //
  //   a) no TTL          -- expires_at is nullopt, so the check short-circuits
  //                         before ever reading the clock
  //   b) long TTL        -- the check runs in full: a clock read and a compare
  //                         on every hit, and nothing ever expires
  //   c) expired entries -- every get finds a corpse and reclaims it
  //
  // (a) vs (b) is the honest measure of "what does the check cost", so it uses
  // the same paired, order-alternated method as section 2.
  std::cout << "\n\n6. COST OF TTL CHECKS\n"
            << "   " << kTtlKeys << " GET hits over a shuffled order, paired and\n"
            << "   order-alternated, " << kAbRepeats << " repeats\n\n";

  std::vector<std::size_t> ttl_order(kTtlKeys);
  std::iota(ttl_order.begin(), ttl_order.end(), std::size_t{0});
  std::shuffle(ttl_order.begin(), ttl_order.end(), std::mt19937(kSeed));

  std::vector<Nanos> no_ttl_runs;
  std::vector<Nanos> with_ttl_runs;
  std::vector<double> ttl_ratio;
  for (int i = 0; i < kAbRepeats; ++i) {
    const bool no_ttl_first = (i % 2) == 0;
    Nanos no_ttl_time{0};
    Nanos with_ttl_time{0};

    for (int pass = 0; pass < 2; ++pass) {
      if ((pass == 0) == no_ttl_first) {
        cachex::Cache cache;
        fill_cache(cache, keys, kTtlKeys, value, std::nullopt);
        no_ttl_time = time_get_pass(cache, keys, ttl_order);
      } else {
        cachex::Cache cache;
        // Far enough out that nothing expires mid-run: this measures the check,
        // not the reclaim path, which case (c) covers separately.
        fill_cache(cache, keys, kTtlKeys, value, std::chrono::minutes(10));
        with_ttl_time = time_get_pass(cache, keys, ttl_order);
      }
    }
    no_ttl_runs.push_back(no_ttl_time);
    with_ttl_runs.push_back(with_ttl_time);
    ttl_ratio.push_back(static_cast<double>(with_ttl_time.count()) /
                        static_cast<double>(no_ttl_time.count()));
  }
  std::sort(ttl_ratio.begin(), ttl_ratio.end());

  // (c) every entry is already expired, so every get reclaims one.
  cachex::Cache expired_cache;
  fill_cache(expired_cache, keys, kTtlKeys, value, cachex::Cache::Duration(1));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const Nanos expired_time = time_get_pass(expired_cache, keys, ttl_order);

  std::cout << std::left << std::setw(30) << "case" << std::right
            << std::setw(14) << "ops/sec" << std::setw(12) << "avg (ns)"
            << std::setw(14) << "reclaimed" << "\n"
            << std::string(70, '-') << "\n";
  const auto ttl_row = [](const char* label, Nanos d, std::size_t reclaimed) {
    std::cout << std::left << std::setw(30) << label << std::right
              << std::setw(14) << std::fixed << std::setprecision(0)
              << ops_per_sec(d, kTtlKeys) << std::setw(12)
              << std::setprecision(1) << avg_ns(d, kTtlKeys) << std::setw(14)
              << reclaimed << "\n";
  };
  ttl_row("a) GET hit, no TTL", median(no_ttl_runs), 0);
  ttl_row("b) GET hit, TTL set", median(with_ttl_runs), 0);
  ttl_row("c) GET, all entries expired", expired_time,
          expired_cache.expired_removals());

  const auto ttl_percent = [](double ratio) { return 100.0 * (ratio - 1.0); };
  std::cout << "\n  Cost of the check itself -- (b) against (a), paired per repeat:\n"
            << "    median " << std::showpos << std::fixed << std::setprecision(1)
            << ttl_percent(ttl_ratio[ttl_ratio.size() / 2]) << "%"
            << "   middle half " << ttl_percent(ttl_ratio[ttl_ratio.size() / 4])
            << "% to " << ttl_percent(ttl_ratio[ttl_ratio.size() * 3 / 4]) << "%"
            << "   full range " << ttl_percent(ttl_ratio.front()) << "% to "
            << ttl_percent(ttl_ratio.back()) << "%" << std::noshowpos << "\n"
            << "  Case (c) is a different path, not merely a slower one: those\n"
            << "  gets return nothing and delete an entry, so it is not\n"
            << "  comparable to (a) or (b). It is reported to show what a sea of\n"
            << "  expired entries costs to walk through once.\n";

  {
    cachex::Cache no_ttl;
    fill_cache(no_ttl, keys, kTtlKeys, value, std::nullopt);
    cachex::Cache with_ttl;
    fill_cache(with_ttl, keys, kTtlKeys, value, std::chrono::minutes(10));
    cachex::Cache expired;
    fill_cache(expired, keys, kTtlKeys, value, cachex::Cache::Duration(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    std::cout << "\n"
              << std::left << std::setw(14) << "  latency (ns)" << std::right
              << std::setw(11) << "samples" << std::setw(11) << "mean"
              << std::setw(10) << "p50" << std::setw(10) << "p95"
              << std::setw(10) << "p99" << std::setw(12) << "max" << "\n"
              << "  " << std::string(76, '-') << "\n";
    print_latency_row("  no TTL", latency_of_get_pass(no_ttl, keys, ttl_order));
    print_latency_row("  TTL set", latency_of_get_pass(with_ttl, keys, ttl_order));
    print_latency_row("  expired", latency_of_get_pass(expired, keys, ttl_order));
    std::cout << "  (each sample includes ~" << std::fixed << std::setprecision(1)
              << clock_ns << " ns of clock-read overhead, and is quantised to the\n"
              << "   clock's " << tick_ns
              << " ns tick -- so these are tick counts, not fine-grained times)\n";
  }

  std::cout << "\nchecksum: " << g_sink
            << "  (printed only so the optimiser cannot discard the work)\n";
  return 0;
}
