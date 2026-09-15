#include <sys/socket.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bench_util.hpp"
#include "cachex/sharded_cache.hpp"
#include "cachex/line_buffer.hpp"
#include "cachex/server.hpp"
#include "cachex/socket.hpp"

namespace {

using bench::Clock;
using bench::Nanos;
using bench::pad_key;
using bench::StartGate;

constexpr std::size_t kRequests = 20000;  // per phase, single-client section

// Divisible by every client count below, so each configuration performs exactly
// the same total work and the throughputs are directly comparable.
constexpr std::size_t kTotalRequests = 32000;
constexpr int kClientCounts[] = {1, 2, 4, 8, 16};
constexpr std::size_t kShardCounts[] = {1, 2, 4, 8};

// The in-process runs need far more work than the networked ones: without a
// ~20 us round trip per request they finish in milliseconds, which is too short
// to measure reliably.
constexpr std::size_t kInProcessOps = 600000;

// Key space for the skewed hit-rate comparison.
constexpr std::size_t kKeySpaceForSkew = 100000;
constexpr std::size_t kSkewWorkloadOps = 200000;
constexpr std::uint32_t kSeed = 42;  // fixed, so the workload is reproducible
constexpr std::size_t kValueBytes = 64;

/// A blocking, one-request-at-a-time client -- no pipelining, no concurrency.
/// That is deliberate: it measures a full round trip per request.
class BenchClient {
 public:
  explicit BenchClient(std::uint16_t port) {
    std::string error;
    socket_ = cachex::connect_to("127.0.0.1", port, error);
    if (!socket_.valid()) {
      std::cerr << "benchmark: cannot connect: " << error << "\n";
    }
  }

  bool connected() const { return socket_.valid(); }

  /// Sends one command and waits for its reply. The returned value is the
  /// reply's first byte, which is enough to keep the optimiser honest without
  /// allocating a string comparison into the measured loop.
  char round_trip(const std::string& command) {
    if (!cachex::send_all(socket_.get(), command)) {
      return '!';
    }
    while (true) {
      if (std::optional<std::string> line = replies_.next_line()) {
        return line->empty() ? '?' : (*line)[0];
      }
      char chunk[4096];
      const ssize_t received = ::recv(socket_.get(), chunk, sizeof(chunk), 0);
      if (received <= 0) {
        return '!';
      }
      replies_.append(chunk, static_cast<std::size_t>(received));
    }
  }

 private:
  cachex::Socket socket_;
  cachex::LineBuffer replies_;
};

struct ConcurrentResult {
  Nanos wall{0};
  bench::Latency latency;
  std::size_t requests = 0;
  std::size_t failures = 0;
};

/// Runs `clients` connections in parallel, each issuing the same number of
/// requests, and reports aggregate throughput plus the merged latency
/// distribution across every request from every client.
ConcurrentResult run_concurrent(
    std::uint16_t port, int clients, std::size_t requests_per_client,
    const std::function<std::string(int, std::size_t)>& make_command) {
  StartGate gate(clients);
  std::vector<std::vector<std::int64_t>> per_client(
      static_cast<std::size_t>(clients));
  std::vector<std::size_t> failures(static_cast<std::size_t>(clients), 0);
  std::vector<std::thread> threads;
  threads.reserve(static_cast<std::size_t>(clients));

  for (int c = 0; c < clients; ++c) {
    threads.emplace_back([&, c] {
      BenchClient client(port);
      std::vector<std::int64_t>& samples = per_client[static_cast<std::size_t>(c)];
      samples.reserve(requests_per_client);

      // Connect first, then wait: connection setup must not land inside the
      // timed window.
      gate.arrive_and_wait();

      for (std::size_t i = 0; i < requests_per_client; ++i) {
        const std::string command = make_command(c, i);
        const auto start = Clock::now();
        const char tag = client.round_trip(command);
        const auto end = Clock::now();
        samples.push_back(std::chrono::duration_cast<Nanos>(end - start).count());
        if (tag == '!' || tag == '-') {
          ++failures[static_cast<std::size_t>(c)];
        }
      }
    });
  }

  gate.wait_until_all_ready();
  const auto wall_start = Clock::now();
  gate.release();
  for (std::thread& thread : threads) {
    thread.join();
  }
  const auto wall_end = Clock::now();

  ConcurrentResult result;
  result.wall = std::chrono::duration_cast<Nanos>(wall_end - wall_start);
  result.requests = requests_per_client * static_cast<std::size_t>(clients);

  std::vector<std::int64_t> merged;
  merged.reserve(result.requests);
  for (const auto& samples : per_client) {
    merged.insert(merged.end(), samples.begin(), samples.end());
  }
  for (const std::size_t f : failures) {
    result.failures += f;
  }
  result.latency = bench::summarize(merged);
  return result;
}

struct PhaseResult {
  Nanos total{0};
  bench::Latency latency;
  std::uint64_t checksum = 0;
};

/// Runs `requests` round trips, timing each one individually.
///
/// Unlike the in-process benchmark, per-request timing is cheap *relative to
/// what is being measured* here: a network round trip is tens of microseconds,
/// three orders of magnitude above the clock's ~41 ns tick, so the percentiles
/// are real measurements rather than tick counts.
PhaseResult run_phase(BenchClient& client, const std::vector<std::string>& commands) {
  PhaseResult result;
  std::vector<std::int64_t> samples;
  samples.reserve(commands.size());

  const auto phase_start = Clock::now();
  for (const std::string& command : commands) {
    const auto start = Clock::now();
    const char tag = client.round_trip(command);
    const auto end = Clock::now();
    samples.push_back(std::chrono::duration_cast<Nanos>(end - start).count());
    result.checksum += static_cast<std::uint64_t>(tag);
  }
  result.total = std::chrono::duration_cast<Nanos>(Clock::now() - phase_start);
  result.latency = bench::summarize(samples);
  return result;
}

void print_phase(const char* name, const PhaseResult& result) {
  std::cout << std::left << std::setw(10) << name << std::right << std::setw(10)
            << result.latency.samples << std::setw(14) << std::fixed
            << std::setprecision(0) << bench::ops_per_sec(result.total, kRequests)
            << std::setw(12) << std::setprecision(2)
            << bench::avg_ns(result.total, kRequests) / 1000.0 << std::setw(11)
            << result.latency.p50 / 1000.0 << std::setw(11)
            << result.latency.p95 / 1000.0 << std::setw(11)
            << result.latency.p99 / 1000.0 << std::setw(12)
            << result.latency.max / 1000.0 << "\n";
}

}  // namespace

int main() {
  std::cout << "CacheX network benchmark\n"
            << "========================\n";

#ifndef NDEBUG
  std::cout << "\n*** WARNING: assertions are enabled -- this is not a Release\n"
            << "*** build. These numbers measure the absence of the optimiser.\n";
#endif

  const double clock_ns = bench::measure_clock_overhead_ns();
  const double tick_ns = bench::measure_clock_granularity_ns();

  // The server runs in this process, on a loopback socket. That keeps the
  // benchmark a single self-contained binary and removes any question of which
  // build of the server is being measured -- but it does mean the numbers
  // include no real network, only the loopback path through the kernel.
  cachex::ShardedCache cache{1};
  cachex::Server::Options options;
  options.port = 0;  // let the OS pick a free port
  options.verbose = false;
  cachex::Server server(cache, options);

  std::string error;
  if (!server.start(error)) {
    std::cerr << "benchmark: " << error << "\n";
    return 1;
  }
  std::thread server_thread([&server] { server.run(); });

  std::cout << "\nbuild        : " << CACHEX_BUILD_TYPE << "\n"
            << "compiler     : " << CACHEX_COMPILER << "\n"
            << "transport    : TCP over loopback, port " << server.bound_port()
            << ", TCP_NODELAY on\n"
            << "clients      : 1 (blocking, one request in flight at a time)\n"
            << "server       : single-threaded, one connection at a time\n"
            << "requests     : " << kRequests << " per phase\n"
            << "value size   : " << kValueBytes << " bytes\n"
            << "clock        : steady_clock, overhead " << std::fixed
            << std::setprecision(1) << clock_ns << " ns/read, tick " << tick_ns
            << " ns\n";

  {
    BenchClient client(server.bound_port());
    if (!client.connected()) {
      server.stop();
      server_thread.join();
      return 1;
    }

    const std::string value(kValueBytes, 'v');

    // Commands are built up front so that string construction is never timed.
    std::vector<std::string> pings(kRequests, "PING\n");
    std::vector<std::string> sets;
    std::vector<std::string> gets;
    sets.reserve(kRequests);
    gets.reserve(kRequests);
    for (std::size_t i = 0; i < kRequests; ++i) {
      sets.push_back("SET " + pad_key(i) + " " + value + "\n");
      gets.push_back("GET " + pad_key(i) + "\n");
    }

    // Warm-up, discarded: the first connection pays for TCP window ramp-up and
    // a cold cache, neither of which is representative of steady state.
    run_phase(client, std::vector<std::string>(pings.begin(), pings.begin() + 2000));

    const PhaseResult ping = run_phase(client, pings);
    const PhaseResult set = run_phase(client, sets);
    const PhaseResult get = run_phase(client, gets);

    std::cout << "\n"
              << std::left << std::setw(10) << "phase" << std::right
              << std::setw(10) << "requests" << std::setw(14) << "req/sec"
              << std::setw(12) << "avg (us)" << std::setw(11) << "p50"
              << std::setw(11) << "p95" << std::setw(11) << "p99"
              << std::setw(12) << "max (us)" << "\n"
              << std::string(91, '-') << "\n";
    print_phase("PING", ping);
    print_phase("SET", set);
    print_phase("GET", get);

    std::cout
        << "\n  PING is the floor: a full round trip that touches no cache data,\n"
        << "  so the gap between it and SET/GET is the cache work, and PING\n"
        << "  itself is what the transport costs.\n"
        << "\n  Percentiles here are real measurements, not tick counts: a round\n"
        << "  trip is tens of microseconds against a " << std::setprecision(0)
        << tick_ns << " ns clock tick -- unlike the\n"
        << "  in-process benchmark, where the tick swamped the operation.\n"
        << "\n  entries in cache: " << cache.size()
        << "   checksum: " << (ping.checksum + set.checksum + get.checksum)
        << "\n";
  }  // the single client disconnects here

  // ======================================================================
  // Sharding matrix. Every configuration replays the identical workload; the
  // only thing that changes is the shard count and the client count.
  // ======================================================================
  {
    const std::string value(kValueBytes, 'v');

    struct Cell {
      double throughput = 0.0;
      double p50 = 0.0;
      double p95 = 0.0;
      double p99 = 0.0;
    };
    // [shard configuration][client count]
    using Matrix = std::vector<std::vector<Cell>>;

    const auto measure_matrix =
        [&](const std::function<std::string(int, std::size_t)>& make_command,
            bool prepopulate) {
          Matrix matrix;
          for (const std::size_t shards : kShardCounts) {
            // A fresh cache and a fresh server per shard count, so no state
            // carries over between configurations.
            cachex::ShardedCache sharded(shards);
            if (prepopulate) {
              for (std::size_t i = 0; i < kTotalRequests; ++i) {
                sharded.set(pad_key(i), value);
              }
            }
            cachex::Server::Options opts;
            opts.port = 0;
            opts.verbose = false;
            cachex::Server shard_server(sharded, opts);
            std::string err;
            if (!shard_server.start(err)) {
              std::cerr << "benchmark: " << err << "\n";
              return matrix;
            }
            std::thread shard_thread([&shard_server] { shard_server.run(); });

            std::vector<Cell> row;
            for (const int clients : kClientCounts) {
              const std::size_t per_client =
                  kTotalRequests / static_cast<std::size_t>(clients);
              const ConcurrentResult r = run_concurrent(
                  shard_server.bound_port(), clients, per_client, make_command);
              Cell cell;
              cell.throughput = bench::ops_per_sec(r.wall, r.requests);
              cell.p50 = r.latency.p50 / 1000.0;
              cell.p95 = r.latency.p95 / 1000.0;
              cell.p99 = r.latency.p99 / 1000.0;
              row.push_back(cell);
            }
            matrix.push_back(row);

            shard_server.stop();
            shard_thread.join();
          }
          return matrix;
        };

    const auto print_matrix = [&](const char* title, const Matrix& matrix) {
      if (matrix.empty()) {
        return;
      }
      std::cout << "\n" << title << " -- throughput (req/sec)\n\n"
                << std::left << std::setw(10) << "clients" << std::right;
      for (const std::size_t shards : kShardCounts) {
        std::cout << std::setw(13) << (std::to_string(shards) + " shard" +
                                       (shards == 1 ? "" : "s"));
      }
      std::cout << std::setw(16) << "best vs 1" << "\n"
                << std::string(10 + 13 * std::size(kShardCounts) + 16, '-') << "\n";

      for (std::size_t c = 0; c < std::size(kClientCounts); ++c) {
        std::cout << std::left << std::setw(10) << kClientCounts[c] << std::right;
        double best = 0.0;
        for (std::size_t s = 0; s < matrix.size(); ++s) {
          std::cout << std::setw(13) << std::fixed << std::setprecision(0)
                    << matrix[s][c].throughput;
          best = std::max(best, matrix[s][c].throughput);
        }
        const double baseline = matrix[0][c].throughput;
        std::cout << std::setw(15) << std::showpos << std::setprecision(1)
                  << (baseline > 0.0 ? 100.0 * (best / baseline - 1.0) : 0.0)
                  << "%" << std::noshowpos << "\n";
      }

      std::cout << "\n" << title << " -- p99 latency (us)\n\n"
                << std::left << std::setw(10) << "clients" << std::right;
      for (const std::size_t shards : kShardCounts) {
        std::cout << std::setw(13) << (std::to_string(shards) + " shard" +
                                       (shards == 1 ? "" : "s"));
      }
      std::cout << std::setw(16) << "best vs 1" << "\n"
                << std::string(10 + 13 * std::size(kShardCounts) + 16, '-') << "\n";
      for (std::size_t c = 0; c < std::size(kClientCounts); ++c) {
        std::cout << std::left << std::setw(10) << kClientCounts[c] << std::right;
        double lowest = matrix[0][c].p99;
        for (std::size_t s = 0; s < matrix.size(); ++s) {
          std::cout << std::setw(13) << std::fixed << std::setprecision(2)
                    << matrix[s][c].p99;
          lowest = std::min(lowest, matrix[s][c].p99);
        }
        const double baseline = matrix[0][c].p99;
        std::cout << std::setw(15) << std::showpos << std::setprecision(1)
                  << (baseline > 0.0 ? 100.0 * (lowest / baseline - 1.0) : 0.0)
                  << "%" << std::noshowpos << "\n";
      }

      std::cout << "\n" << title << " -- scaling vs 1 client, per shard count\n\n"
                << std::left << std::setw(10) << "clients" << std::right;
      for (const std::size_t shards : kShardCounts) {
        std::cout << std::setw(13) << (std::to_string(shards) + " shard" +
                                       (shards == 1 ? "" : "s"));
      }
      std::cout << "\n"
                << std::string(10 + 13 * std::size(kShardCounts), '-') << "\n";
      for (std::size_t c = 0; c < std::size(kClientCounts); ++c) {
        std::cout << std::left << std::setw(10) << kClientCounts[c] << std::right;
        for (std::size_t s = 0; s < matrix.size(); ++s) {
          const double base = matrix[s][0].throughput;
          std::cout << std::setw(12) << std::fixed << std::setprecision(2)
                    << (base > 0.0 ? matrix[s][c].throughput / base : 0.0) << "x";
        }
        std::cout << "\n";
      }
    };

    std::cout << "\n\nSHARDING OVER TCP\n"
              << "=================\n"
              << kTotalRequests
              << " requests per configuration, split across N clients.\n"
              << "Shard counts 1, 2, 4 and 8; one shard is a single global mutex.\n"
              << "Clients and server share this machine's "
              << std::thread::hardware_concurrency() << " hardware threads.\n";

    print_matrix("GET (all hits)",
                 measure_matrix(
                     [](int, std::size_t i) { return "GET " + pad_key(i) + "\n"; },
                     true));

    print_matrix("SET",
                 measure_matrix(
                     [&value](int client, std::size_t i) {
                       return "SET c" + std::to_string(client) + ":" + pad_key(i) +
                              " " + value + "\n";
                     },
                     false));

    // ====================================================================
    // The same matrix with the network removed: N threads calling the cache
    // directly. This is where the lock is actually visible.
    // ====================================================================
    std::cout << "\n\nSHARDING IN-PROCESS (no sockets)\n"
              << "================================\n"
              << "N threads calling ShardedCache::get() directly, "
              << kInProcessOps << " total calls.\n\n"
              << std::left << std::setw(10) << "threads" << std::right;
    for (const std::size_t shards : kShardCounts) {
      std::cout << std::setw(13)
                << (std::to_string(shards) + " shard" + (shards == 1 ? "" : "s"));
    }
    std::cout << std::setw(16) << "best vs 1" << "\n"
              << std::string(10 + 13 * std::size(kShardCounts) + 16, '-') << "\n";

    std::vector<std::vector<double>> in_process;
    std::uint64_t in_process_checksum = 0;
    for (const std::size_t shards : kShardCounts) {
      cachex::ShardedCache sharded(shards);
      for (std::size_t i = 0; i < kTotalRequests; ++i) {
        sharded.set(pad_key(i), value);
      }
      std::vector<double> row;
      for (const int threads : kClientCounts) {
        const std::size_t per_thread =
            kInProcessOps / static_cast<std::size_t>(threads);
        StartGate gate(threads);
        std::vector<std::thread> workers;
        std::atomic<std::uint64_t> sink{0};
        workers.reserve(static_cast<std::size_t>(threads));
        for (int w = 0; w < threads; ++w) {
          workers.emplace_back([&, w] {
            gate.arrive_and_wait();
            std::uint64_t local = 0;
            for (std::size_t i = 0; i < per_thread; ++i) {
              const std::size_t index =
                  (i * 7 + static_cast<std::size_t>(w) * 1013) % kTotalRequests;
              if (const auto found = sharded.get(pad_key(index))) {
                local += found->size();
              }
            }
            sink.fetch_add(local);
          });
        }
        gate.wait_until_all_ready();
        const auto start = Clock::now();
        gate.release();
        for (std::thread& worker : workers) {
          worker.join();
        }
        const auto elapsed = std::chrono::duration_cast<Nanos>(Clock::now() - start);
        in_process_checksum += sink.load();
        row.push_back(bench::ops_per_sec(
            elapsed, per_thread * static_cast<std::size_t>(threads)));
      }
      in_process.push_back(row);
    }

    for (std::size_t c = 0; c < std::size(kClientCounts); ++c) {
      std::cout << std::left << std::setw(10) << kClientCounts[c] << std::right;
      double best = 0.0;
      for (const auto& row : in_process) {
        std::cout << std::setw(13) << std::fixed << std::setprecision(0) << row[c];
        best = std::max(best, row[c]);
      }
      const double baseline = in_process[0][c];
      std::cout << std::setw(15) << std::showpos << std::setprecision(1)
                << (baseline > 0.0 ? 100.0 * (best / baseline - 1.0) : 0.0) << "%"
                << std::noshowpos << "\n";
    }

    // ====================================================================
    // The correctness cost of sharding. LRU is per-shard, not global: each
    // shard evicts its own least-recently-used entry knowing nothing about the
    // others, so a hot key in a crowded shard can be dropped while a colder key
    // in a quiet shard survives. This measures how much that costs in hit rate.
    // ====================================================================
    std::cout << "\n\nGLOBAL LRU vs PER-SHARD LRU (hit rate cost of sharding)\n"
              << "======================================================\n"
              << "Skewed 80/20 workload, cache-aside, single-threaded so only\n"
              << "the eviction policy differs.\n\n"
              << std::left << std::setw(12) << "capacity" << std::right;
    for (const std::size_t shards : kShardCounts) {
      std::cout << std::setw(13)
                << (std::to_string(shards) + " shard" + (shards == 1 ? "" : "s"));
    }
    std::cout << std::setw(14) << "cost" << "\n"
              << std::string(12 + 13 * std::size(kShardCounts) + 14, '-') << "\n";

    {
      const std::vector<bench::Request> skewed = bench::make_workload(
          kSkewWorkloadOps, kKeySpaceForSkew, 90, /*skewed=*/true, kSeed);
      for (const std::size_t percent : {5u, 10u, 20u, 40u}) {
        const std::size_t cap = kKeySpaceForSkew * percent / 100;
        std::cout << std::left << std::setw(12)
                  << (std::to_string(percent) + "% (" + std::to_string(cap) + ")")
                  << std::right;
        double global_rate = 0.0;
        double worst = 100.0;
        for (const std::size_t shards : kShardCounts) {
          cachex::ShardedCache sharded(shards, cap);
          // Warm up, then measure -- the same steady-state method the LRU
          // benchmark uses.
          for (int pass = 0; pass < 2; ++pass) {
            std::size_t hits = 0;
            std::size_t gets = 0;
            for (const bench::Request& r : skewed) {
              if (r.op == bench::Op::Get) {
                ++gets;
                if (sharded.get(pad_key(r.key))) {
                  ++hits;
                } else {
                  sharded.set(pad_key(r.key), value);
                }
              } else {
                sharded.set(pad_key(r.key), value);
              }
            }
            if (pass == 1) {
              const double rate = gets > 0 ? 100.0 * static_cast<double>(hits) /
                                                 static_cast<double>(gets)
                                           : 0.0;
              if (shards == kShardCounts[0]) {
                global_rate = rate;
              }
              worst = std::min(worst, rate);
              std::cout << std::setw(12) << std::fixed << std::setprecision(2)
                        << rate << "%";
            }
          }
        }
        std::cout << std::setw(13) << std::showpos << std::setprecision(2)
                  << (worst - global_rate) << "pp" << std::noshowpos << "\n";
      }
    }
    std::cout << "\n  'cost' is the worst shard count's hit rate minus the\n"
              << "  1-shard (true global LRU) hit rate, in percentage points.\n";

    std::cout << "\n  (checksum " << in_process_checksum << ")\n"
              << "\n  'best vs 1' compares the best shard count against 1 shard at\n"
              << "  the same client count -- positive is better for throughput,\n"
              << "  negative is better for p99 latency.\n";
  }

  server.stop();
  server_thread.join();
  return 0;
}
