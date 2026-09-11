#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "cachex/cache.hpp"
#include "cachex/version.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using Nanos = std::chrono::nanoseconds;

// Fixed so two runs of the same binary, and runs of two different versions of
// the cache, execute exactly the same sequence of operations.
constexpr std::size_t kOperations = 200000;
constexpr std::size_t kValueBytes = 64;
constexpr std::uint32_t kSeed = 42;
constexpr int kRepeats = 5;

// Written to by every phase and printed at the end. Without an observable use of
// the results, the optimiser is free to delete the calls being measured.
std::uint64_t g_sink = 0;

/// Zero-padded to a constant 16 bytes: variable-length keys would make hashing
/// and comparison cost drift over the course of a run. 16 bytes also fits in
/// libc++/libstdc++ small-string storage, so key handling costs no allocation.
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

struct Timings {
  Nanos set_insert{0};
  Nanos set_update{0};
  Nanos get_hit{0};
  Nanos get_miss{0};
  Nanos contains{0};
  Nanos erase{0};
};

Timings run_once(const std::vector<std::string>& keys,
                 const std::vector<std::string>& absent_keys,
                 const std::vector<std::size_t>& access_order,
                 const std::string& value) {
  Timings t;
  cachex::Cache cache;

  // Each phase touches every key exactly once, so all phases share a divisor.
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

/// Median rather than mean: one unlucky run (a scheduler preemption, another
/// process waking up) skews a mean badly and the median not at all.
Nanos median(std::vector<Nanos> samples) {
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2];
}

void print_row(const char* label, Nanos duration, std::size_t ops) {
  const double seconds = std::chrono::duration<double>(duration).count();
  const double ops_per_sec = seconds > 0.0 ? static_cast<double>(ops) / seconds : 0.0;
  const double avg_ns =
      static_cast<double>(duration.count()) / static_cast<double>(ops);

  std::cout << std::left << std::setw(14) << label << std::right
            << std::setw(10) << ops
            << std::setw(12) << std::fixed << std::setprecision(2) << seconds * 1000.0
            << std::setw(14) << std::setprecision(0) << ops_per_sec
            << std::setw(12) << std::setprecision(1) << avg_ns << "\n";
}

}  // namespace

int main() {
  std::cout << "CacheX " << cachex::version_string() << " benchmark\n"
            << "=================================\n";

#ifndef NDEBUG
  std::cout << "\n*** WARNING: assertions are enabled -- this is not a Release\n"
            << "*** build. These numbers measure the absence of the optimiser.\n";
#endif

  std::cout << "\nbuild        : " << CACHEX_BUILD_TYPE << "\n"
            << "compiler     : " << CACHEX_COMPILER << "\n"
            << "operations   : " << kOperations << " per phase\n"
            << "key size     : 16 bytes (fits small-string storage)\n"
            << "value size   : " << kValueBytes << " bytes (heap allocated)\n"
            << "repeats      : " << kRepeats << ", median reported\n"
            << "seed         : " << kSeed << "\n"
            << "clock        : std::chrono::steady_clock\n";

  // Built once, outside every timed region: key construction is not part of what
  // is being measured.
  std::vector<std::string> keys;
  std::vector<std::string> absent_keys;
  keys.reserve(kOperations);
  absent_keys.reserve(kOperations);
  for (std::size_t i = 0; i < kOperations; ++i) {
    keys.push_back(make_key("key:", i));
    absent_keys.push_back(make_key("nil:", i));
  }

  // Reads follow a shuffled order rather than insertion order. Walking keys in
  // the order they were inserted is unrealistically friendly to the CPU cache
  // and would overstate GET throughput.
  std::vector<std::size_t> access_order(kOperations);
  std::iota(access_order.begin(), access_order.end(), std::size_t{0});
  std::shuffle(access_order.begin(), access_order.end(), std::mt19937(kSeed));

  const std::string value(kValueBytes, 'v');

  // Discarded: the first run pays for page faults and a cold CPU cache.
  run_once(keys, absent_keys, access_order, value);

  std::vector<Timings> runs;
  runs.reserve(static_cast<std::size_t>(kRepeats));
  for (int i = 0; i < kRepeats; ++i) {
    runs.push_back(run_once(keys, absent_keys, access_order, value));
  }

  const auto collect = [&runs](Nanos Timings::*field) {
    std::vector<Nanos> samples;
    samples.reserve(runs.size());
    for (const Timings& run : runs) {
      samples.push_back(run.*field);
    }
    return median(std::move(samples));
  };

  std::cout << "\n"
            << std::left << std::setw(14) << "phase" << std::right
            << std::setw(10) << "ops" << std::setw(12) << "total (ms)"
            << std::setw(14) << "ops/sec" << std::setw(12) << "avg (ns)" << "\n"
            << std::string(62, '-') << "\n";

  print_row("SET insert", collect(&Timings::set_insert), kOperations);
  print_row("SET update", collect(&Timings::set_update), kOperations);
  print_row("GET hit", collect(&Timings::get_hit), kOperations);
  print_row("GET miss", collect(&Timings::get_miss), kOperations);
  print_row("CONTAINS", collect(&Timings::contains), kOperations);
  print_row("ERASE", collect(&Timings::erase), kOperations);

  std::cout << "\nchecksum: " << g_sink
            << "  (printed only so the optimiser cannot discard the work)\n";
  return 0;
}
