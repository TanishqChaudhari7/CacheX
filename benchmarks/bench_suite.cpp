// CacheX benchmark suite.
//
// Runs a fixed set of workloads against three versions of the cache and reports
// the same metrics for each, so the numbers are comparable by construction.
//
//   A  baseline    Cache          single thread, no locking at all
//   B  concurrent  SyncCache      one global mutex
//   C  optimised   ShardedCache   8 independently locked shards
//
// Every workload is generated once from a fixed seed and replayed identically by
// every version, so a difference between rows is a difference in the cache and
// not in what was asked of it.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "bench_util.hpp"
#include "cachex/cache.hpp"
#include "cachex/persistence.hpp"
#include "cachex/sharded_cache.hpp"
#include "cachex/sync_cache.hpp"

#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__linux__)
#include <malloc.h>
#endif

namespace {

using bench::Clock;
using bench::Nanos;

constexpr std::uint32_t kSeed = 20260911;
constexpr std::size_t kOpsPerWorkload = 400000;
constexpr std::size_t kValueBytes = 64;
constexpr int kThreadCounts[] = {1, 2, 4, 8, 16};
constexpr std::size_t kShards = 8;

// --- environment -----------------------------------------------------------

/// Bytes currently handed out by malloc, or 0 if it cannot be determined.
///
/// Deliberately *not* resident set size. RSS does not shrink when memory is
/// freed -- the allocator keeps the pages -- so an RSS delta reads as zero
/// whenever a previous allocation already grew the heap far enough. The first
/// version of this measurement did exactly that and reported 0 bytes/entry for
/// 100k entries.
///
/// Heap bytes-in-use is the allocator's own accounting: it rises and falls with
/// actual allocations, so a before/after delta measures the data structure
/// rather than the process's high-water mark.
std::size_t heap_bytes_in_use() {
#if defined(__APPLE__)
  malloc_statistics_t stats{};
  malloc_zone_statistics(malloc_default_zone(), &stats);
  return static_cast<std::size_t>(stats.size_in_use);
#elif defined(__linux__)
  const struct mallinfo2 info = mallinfo2();
  return static_cast<std::size_t>(info.uordblks);
#else
  return 0;
#endif
}

// --- workloads -------------------------------------------------------------

struct Operation {
  enum Kind : std::uint8_t { Get, Set };
  Kind kind = Get;
  std::uint32_t key = 0;
};

struct WorkloadSpec {
  const char* name;
  const char* description;
  int get_percent;
  std::size_t key_space;
  bool skewed;                                    ///< 80% of traffic to 20% of keys
  std::optional<std::size_t> capacity;            ///< nullopt = unbounded
  std::optional<std::chrono::milliseconds> ttl;   ///< applied to every SET
  bool cache_aside;                               ///< a GET miss populates
};

const WorkloadSpec kWorkloads[] = {
    {"read-heavy", "90% GET / 10% SET, skewed keys", 90, 100000, true, 40000,
     std::nullopt, true},
    {"balanced", "50% GET / 50% SET, skewed keys", 50, 100000, true, 40000,
     std::nullopt, true},
    {"write-heavy", "10% GET / 90% SET, skewed keys", 10, 100000, true, 40000,
     std::nullopt, true},
    {"high-churn", "20% GET / 80% SET, uniform over 10x capacity", 20, 200000,
     false, 20000, std::nullopt, true},
    {"ttl-heavy", "70% GET / 30% SET, every SET carries a 2s TTL", 70, 100000,
     true, 40000, std::chrono::milliseconds(2000), true},
};

/// Generated once per workload from a fixed seed, then replayed by every
/// version. RNG cost is never inside a timed region.
std::vector<Operation> generate(const WorkloadSpec& spec) {
  std::mt19937 rng(kSeed);
  std::uniform_int_distribution<int> roll(1, 100);
  std::uniform_int_distribution<std::uint32_t> any_key(
      0, static_cast<std::uint32_t>(spec.key_space - 1));
  const std::size_t hot = std::max<std::size_t>(1, spec.key_space / 5);
  std::uniform_int_distribution<std::uint32_t> hot_key(
      0, static_cast<std::uint32_t>(hot - 1));
  std::uniform_int_distribution<std::uint32_t> cold_key(
      static_cast<std::uint32_t>(hot),
      static_cast<std::uint32_t>(spec.key_space - 1));

  std::vector<Operation> ops;
  ops.reserve(kOpsPerWorkload);
  for (std::size_t i = 0; i < kOpsPerWorkload; ++i) {
    Operation op;
    op.kind = roll(rng) <= spec.get_percent ? Operation::Get : Operation::Set;
    op.key = spec.skewed ? (roll(rng) <= 80 ? hot_key(rng) : cold_key(rng))
                         : any_key(rng);
    ops.push_back(op);
  }
  return ops;
}

// --- metrics ---------------------------------------------------------------

struct Metrics {
  std::string version;
  std::string workload;
  int threads = 0;
  std::size_t total_ops = 0;
  std::size_t hits = 0;
  std::size_t misses = 0;
  std::size_t sets = 0;
  std::size_t errors = 0;
  std::size_t evictions = 0;
  std::size_t expired = 0;
  std::size_t final_size = 0;
  Nanos wall{0};
  bench::Latency latency;

  double ops_per_sec() const { return bench::ops_per_sec(wall, total_ops); }
  double avg_ns() const { return bench::avg_ns(wall, total_ops); }
  double hit_ratio() const {
    const std::size_t gets = hits + misses;
    return gets > 0 ? 100.0 * static_cast<double>(hits) / static_cast<double>(gets)
                    : 0.0;
  }
  double miss_ratio() const { return hits + misses > 0 ? 100.0 - hit_ratio() : 0.0; }
};

/// Releases every worker at the same instant, so an N-thread run really has N
/// threads in flight rather than a staggered ramp.
class StartGate {
 public:
  explicit StartGate(int participants) : remaining_(participants) {}

  void arrive_and_wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (--remaining_ == 0) {
      ready_.notify_all();
    }
    ready_.wait(lock, [this] { return remaining_ == 0; });
    go_.wait(lock, [this] { return released_; });
  }
  void wait_until_all_ready() {
    std::unique_lock<std::mutex> lock(mutex_);
    ready_.wait(lock, [this] { return remaining_ == 0; });
  }
  void release() {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
    }
    go_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable ready_;
  std::condition_variable go_;
  int remaining_;
  bool released_ = false;
};

std::uint64_t g_sink = 0;

/// Runs one workload against one cache with `threads` workers.
///
/// Templated on the cache type rather than using an interface: Cache, SyncCache
/// and ShardedCache already expose the same operations, and a virtual call on
/// every operation would be measuring the benchmark's own indirection.
template <typename CacheT>
Metrics run(CacheT& cache, const WorkloadSpec& spec,
            const std::vector<Operation>& ops,
            const std::vector<std::string>& keys, const std::string& value,
            const char* version, int threads) {
  const std::size_t per_thread = ops.size() / static_cast<std::size_t>(threads);

  StartGate gate(threads);
  std::vector<std::vector<std::int64_t>> samples(
      static_cast<std::size_t>(threads));
  std::vector<std::size_t> hits(static_cast<std::size_t>(threads), 0);
  std::vector<std::size_t> misses(static_cast<std::size_t>(threads), 0);
  std::vector<std::size_t> sets(static_cast<std::size_t>(threads), 0);
  std::vector<std::size_t> errors(static_cast<std::size_t>(threads), 0);
  std::atomic<std::uint64_t> sink{0};

  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(threads));
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&, t] {
      const std::size_t begin = per_thread * static_cast<std::size_t>(t);
      const std::size_t end = begin + per_thread;
      auto& local_samples = samples[static_cast<std::size_t>(t)];
      local_samples.reserve(per_thread);
      std::size_t local_hits = 0;
      std::size_t local_misses = 0;
      std::size_t local_sets = 0;
      std::size_t local_errors = 0;
      std::uint64_t local_sink = 0;

      gate.arrive_and_wait();

      for (std::size_t i = begin; i < end; ++i) {
        const Operation& op = ops[i];
        const std::string& key = keys[op.key];
        const auto started = Clock::now();
        if (op.kind == Operation::Get) {
          const std::optional<std::string> found = cache.get(key);
          if (found.has_value()) {
            ++local_hits;
            local_sink += found->size();
            if (found->size() != kValueBytes) {
              ++local_errors;  // a value that is not what was stored
            }
          } else {
            ++local_misses;
            if (spec.cache_aside) {
              if (spec.ttl.has_value()) {
                cache.set(key, value, *spec.ttl);
              } else {
                cache.set(key, value);
              }
            }
          }
        } else {
          if (spec.ttl.has_value()) {
            cache.set(key, value, *spec.ttl);
          } else {
            cache.set(key, value);
          }
          ++local_sets;
        }
        const auto finished = Clock::now();
        local_samples.push_back(
            std::chrono::duration_cast<Nanos>(finished - started).count());
      }

      hits[static_cast<std::size_t>(t)] = local_hits;
      misses[static_cast<std::size_t>(t)] = local_misses;
      sets[static_cast<std::size_t>(t)] = local_sets;
      errors[static_cast<std::size_t>(t)] = local_errors;
      sink.fetch_add(local_sink);
    });
  }

  gate.wait_until_all_ready();
  const auto wall_start = Clock::now();
  gate.release();
  for (std::thread& worker : workers) {
    worker.join();
  }
  const auto wall_end = Clock::now();

  Metrics metrics;
  metrics.version = version;
  metrics.workload = spec.name;
  metrics.threads = threads;
  metrics.wall = std::chrono::duration_cast<Nanos>(wall_end - wall_start);
  metrics.total_ops = per_thread * static_cast<std::size_t>(threads);
  for (int t = 0; t < threads; ++t) {
    metrics.hits += hits[static_cast<std::size_t>(t)];
    metrics.misses += misses[static_cast<std::size_t>(t)];
    metrics.sets += sets[static_cast<std::size_t>(t)];
    metrics.errors += errors[static_cast<std::size_t>(t)];
  }
  metrics.evictions = cache.evictions();
  metrics.expired = cache.expired_removals();
  metrics.final_size = cache.size();
  g_sink += sink.load();

  std::vector<std::int64_t> merged;
  merged.reserve(metrics.total_ops);
  for (auto& per_thread_samples : samples) {
    merged.insert(merged.end(), per_thread_samples.begin(),
                  per_thread_samples.end());
  }
  metrics.latency = bench::summarize(merged);
  return metrics;
}

void print_header() {
  std::cout << std::left << std::setw(12) << "version" << std::setw(9)
            << "threads" << std::right << std::setw(11) << "ops" << std::setw(12)
            << "ops/sec" << std::setw(10) << "avg ns" << std::setw(9) << "p50"
            << std::setw(9) << "p95" << std::setw(9) << "p99" << std::setw(9)
            << "hit %" << std::setw(11) << "evictions" << std::setw(9)
            << "errors" << "\n"
            << std::string(110, '-') << "\n";
}

void print_row(const Metrics& m) {
  std::cout << std::left << std::setw(12) << m.version << std::setw(9)
            << m.threads << std::right << std::setw(11) << m.total_ops
            << std::setw(12) << std::fixed << std::setprecision(0)
            << m.ops_per_sec() << std::setw(10) << std::setprecision(1)
            << m.avg_ns() << std::setw(9) << std::setprecision(0)
            << m.latency.p50 << std::setw(9) << m.latency.p95 << std::setw(9)
            << m.latency.p99 << std::setw(9) << std::setprecision(2)
            << m.hit_ratio() << std::setw(11) << m.evictions << std::setw(9)
            << m.errors << "\n";
}

}  // namespace

int main() {
  std::cout << "CacheX benchmark suite\n"
            << "======================\n";

#ifndef NDEBUG
  std::cout << "\n*** WARNING: assertions are enabled -- this is not a Release\n"
            << "*** build. These numbers measure the absence of the optimiser.\n";
#endif

  const double clock_ns = bench::measure_clock_overhead_ns();
  const double tick_ns = bench::measure_clock_granularity_ns();

  std::cout << "\nbuild        : " << CACHEX_BUILD_TYPE << "\n"
            << "compiler     : " << CACHEX_COMPILER << "\n"
            << "hw threads   : " << std::thread::hardware_concurrency() << "\n"
            << "seed         : " << kSeed << " (fixed; every version replays the "
            << "identical operation sequence)\n"
            << "operations   : " << kOpsPerWorkload << " per workload\n"
            << "value size   : " << kValueBytes << " bytes\n"
            << "shards (C)   : " << kShards << "\n"
            << "clock        : steady_clock, overhead " << std::fixed
            << std::setprecision(1) << clock_ns << " ns/read, tick " << tick_ns
            << " ns\n";

  std::cout << "\n  NOTE: per-operation latencies are quantised to the " << tick_ns
            << " ns clock tick.\n"
            << "  Operations here take a few hundred ns, so p50/p95/p99 are\n"
            << "  accurate to roughly one tick. ops/sec and avg ns come from the\n"
            << "  wall time of the whole run and are not tick-limited.\n";

  // Keys are built once, outside every timed region.
  std::size_t widest_key_space = 0;
  for (const WorkloadSpec& spec : kWorkloads) {
    widest_key_space = std::max(widest_key_space, spec.key_space);
  }
  std::vector<std::string> keys;
  keys.reserve(widest_key_space);
  for (std::size_t i = 0; i < widest_key_space; ++i) {
    const std::string digits = std::to_string(i);
    keys.push_back("key:" + std::string(12 - digits.size(), '0') + digits);
  }
  const std::string value(kValueBytes, 'v');

  std::vector<Metrics> all;

  for (const WorkloadSpec& spec : kWorkloads) {
    const std::vector<Operation> ops = generate(spec);

    std::cout << "\n\nWORKLOAD: " << spec.name << "\n"
              << spec.description << "\n"
              << "key space " << spec.key_space << ", capacity "
              << (spec.capacity ? std::to_string(*spec.capacity)
                                : std::string("unbounded"))
              << ", cache-aside " << (spec.cache_aside ? "on" : "off") << "\n\n";
    print_header();

    // A -- baseline: the raw Cache, single threaded, no locking anywhere.
    {
      cachex::Cache cache =
          spec.capacity ? cachex::Cache(*spec.capacity) : cachex::Cache();
      const Metrics m = run(cache, spec, ops, keys, value, "A baseline", 1);
      print_row(m);
      all.push_back(m);
    }

    // B -- one global mutex.
    for (const int threads : kThreadCounts) {
      cachex::SyncCache cache = spec.capacity ? cachex::SyncCache(*spec.capacity)
                                              : cachex::SyncCache();
      const Metrics m = run(cache, spec, ops, keys, value, "B mutex", threads);
      print_row(m);
      all.push_back(m);
    }

    // C -- sharded.
    for (const int threads : kThreadCounts) {
      cachex::ShardedCache cache =
          spec.capacity ? cachex::ShardedCache(kShards, *spec.capacity)
                        : cachex::ShardedCache(kShards);
      const Metrics m = run(cache, spec, ops, keys, value, "C sharded", threads);
      print_row(m);
      all.push_back(m);
    }
  }

  // --- comparison summary ---------------------------------------------------
  std::cout << "\n\nVERSION COMPARISON\n"
            << "==================\n"
            << "B and C at their best thread count, against the single-threaded\n"
            << "baseline and against each other.\n\n"
            << std::left << std::setw(14) << "workload" << std::right
            << std::setw(13) << "A (1 thr)" << std::setw(13) << "B best"
            << std::setw(8) << "at" << std::setw(13) << "C best" << std::setw(8)
            << "at" << std::setw(12) << "C vs B" << std::setw(12) << "C vs A"
            << "\n"
            << std::string(93, '-') << "\n";

  for (const WorkloadSpec& spec : kWorkloads) {
    double a = 0.0;
    double best_b = 0.0;
    double best_c = 0.0;
    int b_threads = 0;
    int c_threads = 0;
    for (const Metrics& m : all) {
      if (m.workload != spec.name) {
        continue;
      }
      if (m.version[0] == 'A') {
        a = m.ops_per_sec();
      } else if (m.version[0] == 'B' && m.ops_per_sec() > best_b) {
        best_b = m.ops_per_sec();
        b_threads = m.threads;
      } else if (m.version[0] == 'C' && m.ops_per_sec() > best_c) {
        best_c = m.ops_per_sec();
        c_threads = m.threads;
      }
    }
    std::cout << std::left << std::setw(14) << spec.name << std::right
              << std::setw(13) << std::fixed << std::setprecision(0) << a
              << std::setw(13) << best_b << std::setw(6) << b_threads << "t"
              << std::setw(13) << best_c << std::setw(6) << c_threads << "t"
              << std::setw(11) << std::showpos << std::setprecision(1)
              << (best_b > 0.0 ? 100.0 * (best_c / best_b - 1.0) : 0.0) << "%"
              << std::setw(11) << (a > 0.0 ? 100.0 * (best_c / a - 1.0) : 0.0)
              << "%" << std::noshowpos << "\n";
  }

  // The table above picks each version's best thread count, which for both B
  // and C turns out to be 1 -- so it answers "does adding threads help at all?"
  // and not "is sharding better than a global mutex?". This table answers the
  // second question, comparing them at the same thread count.
  std::cout << "\n\nC vs B AT MATCHED THREAD COUNTS (throughput)\n"
            << "===========================================\n"
            << "The fair comparison for the sharding question: same workload,\n"
            << "same number of threads, only the locking strategy differs.\n\n"
            << std::left << std::setw(14) << "workload" << std::right;
  for (const int threads : kThreadCounts) {
    std::cout << std::setw(11) << (std::to_string(threads) + " thread" +
                                   (threads == 1 ? "" : "s"));
  }
  std::cout << "\n" << std::string(14 + 11 * std::size(kThreadCounts), '-') << "\n";
  for (const WorkloadSpec& spec : kWorkloads) {
    std::cout << std::left << std::setw(14) << spec.name << std::right;
    for (const int threads : kThreadCounts) {
      double b = 0.0;
      double c = 0.0;
      for (const Metrics& m : all) {
        if (m.workload == spec.name && m.threads == threads) {
          if (m.version[0] == 'B') {
            b = m.ops_per_sec();
          } else if (m.version[0] == 'C') {
            c = m.ops_per_sec();
          }
        }
      }
      std::cout << std::setw(10) << std::showpos << std::fixed
                << std::setprecision(1)
                << (b > 0.0 ? 100.0 * (c / b - 1.0) : 0.0) << "%"
                << std::noshowpos;
    }
    std::cout << "\n";
  }

  // --- p99 comparison -------------------------------------------------------
  std::cout << "\n\np99 LATENCY, B vs C at each thread count (ns)\n"
            << "=============================================\n\n"
            << std::left << std::setw(14) << "workload" << std::right;
  for (const int threads : kThreadCounts) {
    std::cout << std::setw(9) << (std::to_string(threads) + "t B")
              << std::setw(9) << (std::to_string(threads) + "t C");
  }
  std::cout << "\n" << std::string(14 + 18 * std::size(kThreadCounts), '-') << "\n";
  for (const WorkloadSpec& spec : kWorkloads) {
    std::cout << std::left << std::setw(14) << spec.name << std::right;
    for (const int threads : kThreadCounts) {
      double b = 0.0;
      double c = 0.0;
      for (const Metrics& m : all) {
        if (m.workload == spec.name && m.threads == threads) {
          if (m.version[0] == 'B') {
            b = m.latency.p99;
          } else if (m.version[0] == 'C') {
            c = m.latency.p99;
          }
        }
      }
      std::cout << std::setw(9) << std::fixed << std::setprecision(0) << b
                << std::setw(9) << c;
    }
    std::cout << "\n";
  }

  std::cout << "\np99 reduction, C against B at the same thread count:\n\n"
            << std::left << std::setw(14) << "workload" << std::right;
  for (const int threads : kThreadCounts) {
    std::cout << std::setw(11) << (std::to_string(threads) + " thread" +
                                   (threads == 1 ? "" : "s"));
  }
  std::cout << "\n" << std::string(14 + 11 * std::size(kThreadCounts), '-') << "\n";
  for (const WorkloadSpec& spec : kWorkloads) {
    std::cout << std::left << std::setw(14) << spec.name << std::right;
    for (const int threads : kThreadCounts) {
      double b = 0.0;
      double c = 0.0;
      for (const Metrics& m : all) {
        if (m.workload == spec.name && m.threads == threads) {
          if (m.version[0] == 'B') {
            b = m.latency.p99;
          } else if (m.version[0] == 'C') {
            c = m.latency.p99;
          }
        }
      }
      std::cout << std::setw(10) << std::showpos << std::fixed
                << std::setprecision(1)
                << (b > 0.0 ? 100.0 * (c / b - 1.0) : 0.0) << "%"
                << std::noshowpos;
    }
    std::cout << "\n";
  }
  std::cout << "\n  Negative is better: C's p99 is lower than B's by that much.\n";

  // --- memory ---------------------------------------------------------------
  std::cout << "\n\nMEMORY PER ENTRY (measured, not estimated)\n"
            << "==========================================\n\n"
            << std::left << std::setw(12) << "entries" << std::right
            << std::setw(16) << "heap delta" << std::setw(16) << "bytes/entry"
            << std::setw(16) << "vs payload" << "\n"
            << std::string(60, '-') << "\n";

  const std::size_t payload = 16 + kValueBytes;
  for (const std::size_t entries : {100000u, 250000u, 500000u}) {
    const std::size_t before = heap_bytes_in_use();
    {
      auto cache = std::make_unique<cachex::Cache>();
      for (std::size_t i = 0; i < entries; ++i) {
        const std::string digits = std::to_string(i);
        cache->set("key:" + std::string(12 - digits.size(), '0') + digits, value);
      }
      const std::size_t after = heap_bytes_in_use();
      const double delta = static_cast<double>(after) - static_cast<double>(before);
      const double per_entry = delta / static_cast<double>(entries);
      std::cout << std::left << std::setw(12) << entries << std::right
                << std::setw(14) << std::fixed << std::setprecision(1)
                << delta / (1024.0 * 1024.0) << " MiB" << std::setw(16)
                << std::setprecision(1) << per_entry << std::setw(15)
                << std::setprecision(2)
                << per_entry / static_cast<double>(payload) << "x" << "\n";
      g_sink += cache->size();
    }
    // The cache is destroyed here, so the next iteration starts from the same
    // baseline rather than accumulating.
  }
  std::cout << "\n  payload (16-byte key + " << kValueBytes
            << "-byte value) = " << payload << " bytes/entry.\n"
            << "  Measured as malloc bytes-in-use before and after building the\n"
            << "  cache, so it covers the two node allocations per entry, the\n"
            << "  value's heap buffer, the duplicated key and the bucket array --\n"
            << "  everything the data structure asks the allocator for. It does\n"
            << "  not include allocator slack within a size class, so the true\n"
            << "  cost to the process is somewhat higher.\n";

  // --- optimisation: before / after -----------------------------------------
  //
  // Profiling (macOS `sample`, 8 s of the suite) put mutex contention first by a
  // wide margin, and malloc/free second. The contention is what sharding already
  // addresses. The allocation is new: get() returns std::optional<std::string>,
  // so every hit on a value past the small-string limit is one malloc and one
  // free. get_into() copies into a caller-owned buffer instead.
  //
  // Same workload, same cache, single-threaded so no contention is mixed in --
  // only the return path differs.
  std::cout << "\n\nOPTIMISATION: get() vs get_into()\n"
            << "=================================\n"
            << "Single-threaded, read-heavy workload, " << kValueBytes
            << "-byte values.\n"
            << "get() allocates a std::string per hit; get_into() reuses one "
            << "buffer.\n\n"
            << std::left << std::setw(16) << "variant" << std::right
            << std::setw(13) << "ops/sec" << std::setw(11) << "avg ns"
            << std::setw(10) << "p50" << std::setw(10) << "p99"
            << std::setw(14) << "heap bytes" << "\n"
            << std::string(74, '-') << "\n";

  {
    const WorkloadSpec& spec = kWorkloads[0];  // read-heavy
    const std::vector<Operation> ops = generate(spec);

    const auto run_variant = [&](bool use_get_into) {
      cachex::Cache cache(*spec.capacity);
      std::vector<std::int64_t> samples;
      samples.reserve(ops.size());
      std::string buffer;
      std::uint64_t local_sink = 0;
      std::size_t hits = 0;

      const std::size_t heap_before = heap_bytes_in_use();
      const auto start = Clock::now();
      for (const Operation& op : ops) {
        const std::string& key = keys[op.key];
        const auto op_start = Clock::now();
        if (op.kind == Operation::Get) {
          if (use_get_into) {
            if (cache.get_into(key, buffer)) {
              ++hits;
              local_sink += buffer.size();
            } else {
              cache.set(key, value);
            }
          } else {
            if (const std::optional<std::string> found = cache.get(key)) {
              ++hits;
              local_sink += found->size();
            } else {
              cache.set(key, value);
            }
          }
        } else {
          cache.set(key, value);
        }
        samples.push_back(
            std::chrono::duration_cast<Nanos>(Clock::now() - op_start).count());
      }
      const auto elapsed = std::chrono::duration_cast<Nanos>(Clock::now() - start);
      const std::size_t heap_after = heap_bytes_in_use();
      g_sink += local_sink + hits;

      const bench::Latency latency = bench::summarize(samples);
      std::cout << std::left << std::setw(16)
                << (use_get_into ? "get_into()" : "get()") << std::right
                << std::setw(13) << std::fixed << std::setprecision(0)
                << bench::ops_per_sec(elapsed, ops.size()) << std::setw(11)
                << std::setprecision(1) << bench::avg_ns(elapsed, ops.size())
                << std::setw(10) << std::setprecision(0) << latency.p50
                << std::setw(10) << latency.p99 << std::setw(14)
                << (heap_after > heap_before ? heap_after - heap_before : 0)
                << "\n";
      return bench::ops_per_sec(elapsed, ops.size());
    };

    const double before = run_variant(false);
    const double after = run_variant(true);
    std::cout << "\n  throughput change: " << std::showpos << std::fixed
              << std::setprecision(1)
              << (before > 0.0 ? 100.0 * (after / before - 1.0) : 0.0) << "%"
              << std::noshowpos << "\n";
  }

  std::cout << "\nchecksum: " << g_sink
            << "  (printed only so the optimiser cannot discard the work)\n";
  return 0;
}
