#pragma once

// Timing and statistics shared by the benchmarks. Header-only so each benchmark
// stays a single translation unit.

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace bench {

using Clock = std::chrono::steady_clock;
using Nanos = std::chrono::nanoseconds;

struct Latency {
  double mean = 0.0;
  double p50 = 0.0;
  double p95 = 0.0;
  double p99 = 0.0;
  double max = 0.0;
  std::size_t samples = 0;
};

/// Sorts in place, then indexes. Nearest-rank; with tens of thousands of
/// samples the choice of interpolation rule makes no visible difference.
inline Latency summarize(std::vector<std::int64_t>& ns) {
  Latency out;
  out.samples = ns.size();
  if (ns.empty()) {
    return out;
  }
  std::sort(ns.begin(), ns.end());

  const auto at = [&ns](double p) {
    const auto last = static_cast<double>(ns.size() - 1);
    return static_cast<double>(ns[static_cast<std::size_t>(p * last)]);
  };
  double sum = 0.0;
  for (const std::int64_t v : ns) {
    sum += static_cast<double>(v);
  }
  out.mean = sum / static_cast<double>(ns.size());
  out.p50 = at(0.50);
  out.p95 = at(0.95);
  out.p99 = at(0.99);
  out.max = static_cast<double>(ns.back());
  return out;
}

/// Median rather than mean: one unlucky run (a scheduler preemption, a
/// background process) skews a mean badly and a median not at all.
inline Nanos median(std::vector<Nanos> samples) {
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2];
}

inline double ops_per_sec(Nanos duration, std::size_t ops) {
  const double seconds = std::chrono::duration<double>(duration).count();
  return seconds > 0.0 ? static_cast<double>(ops) / seconds : 0.0;
}

inline double avg_ns(Nanos duration, std::size_t ops) {
  return ops > 0 ? static_cast<double>(duration.count()) / static_cast<double>(ops)
                 : 0.0;
}

/// The cost of one steady_clock::now() call, measured rather than assumed.
inline double measure_clock_overhead_ns() {
  constexpr int kCalibrationReads = 500000;
  // volatile, rather than feeding a checksum: the accumulator must be
  // observable so the loop survives -O3, but its value is timing-dependent and
  // would make a reported checksum differ between runs.
  volatile std::int64_t acc = 0;
  const auto start = Clock::now();
  for (int i = 0; i < kCalibrationReads; ++i) {
    acc = acc + Clock::now().time_since_epoch().count();
  }
  const auto elapsed = std::chrono::duration_cast<Nanos>(Clock::now() - start);
  return static_cast<double>(elapsed.count()) / kCalibrationReads;
}

/// The smallest non-zero interval the clock can report -- its tick. Per-operation
/// latencies are quantised to a multiple of this, so on hardware where the tick
/// is comparable to the operation being timed (Apple Silicon ticks at ~41 ns),
/// a "p50 of 42 ns" means "one tick", not a measurement of 42 ns.
inline double measure_clock_granularity_ns() {
  std::int64_t smallest = std::numeric_limits<std::int64_t>::max();
  for (int i = 0; i < 200000; ++i) {
    const auto a = Clock::now();
    const auto b = Clock::now();
    const std::int64_t delta = std::chrono::duration_cast<Nanos>(b - a).count();
    if (delta > 0 && delta < smallest) {
      smallest = delta;
    }
  }
  return smallest == std::numeric_limits<std::int64_t>::max()
             ? 0.0
             : static_cast<double>(smallest);
}

inline void print_latency_header(const char* label) {
  std::cout << std::left << std::setw(14) << label << std::right << std::setw(11)
            << "samples" << std::setw(11) << "mean" << std::setw(10) << "p50"
            << std::setw(10) << "p95" << std::setw(10) << "p99" << std::setw(12)
            << "max" << "\n"
            << "  " << std::string(76, '-') << "\n";
}

inline void print_latency_row(const char* label, const Latency& l) {
  std::cout << std::left << std::setw(14) << label << std::right << std::setw(11)
            << l.samples << std::fixed << std::setprecision(1) << std::setw(11)
            << l.mean << std::setw(10) << l.p50 << std::setw(10) << l.p95
            << std::setw(10) << l.p99 << std::setw(12) << l.max << "\n";
}

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
inline std::vector<Request> make_workload(std::size_t ops, std::size_t key_space,
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

/// A zero-padded key such as "key:000000000042": always 16 bytes with the
/// default prefix. Constant length keeps hashing cost from drifting during a
/// run, and 16 bytes fits small-string storage, so building or comparing a key
/// costs no allocation.
inline std::string pad_key(std::size_t i, const char* prefix = "key:") {
  const std::string digits = std::to_string(i);
  return prefix + std::string(12 - digits.size(), '0') + digits;
}

/// Releases every worker thread at the same instant.
///
/// Without it the first thread would start, and perhaps finish, while the last
/// was still setting up, so an "N thread" run would spend part of its time with
/// fewer than N in flight -- understating contention exactly where the
/// benchmark is trying to measure it.
class StartGate {
 public:
  explicit StartGate(int participants) : remaining_(participants) {}

  /// Called by each worker once it is ready; returns when release() is called.
  void arrive_and_wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (--remaining_ == 0) {
      ready_.notify_all();
    }
    ready_.wait(lock, [this] { return remaining_ == 0; });
    go_.wait(lock, [this] { return released_; });
  }

  /// Blocks until every worker has arrived.
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

}  // namespace bench
