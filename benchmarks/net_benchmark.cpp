#include <sys/socket.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bench_util.hpp"
#include "cachex/sync_cache.hpp"
#include "cachex/line_buffer.hpp"
#include "cachex/server.hpp"
#include "cachex/socket.hpp"

namespace {

using bench::Clock;
using bench::Nanos;

constexpr std::size_t kRequests = 20000;  // per phase, single-client section

// Divisible by every client count below, so each configuration performs exactly
// the same total work and the throughputs are directly comparable.
constexpr std::size_t kTotalRequests = 32000;
constexpr int kClientCounts[] = {1, 2, 4, 8, 16};
constexpr std::size_t kValueBytes = 64;

/// A blocking, one-request-at-a-time client -- no pipelining, no concurrency.
/// That is deliberate: it measures a full round trip per request, which is the
/// baseline the later stages get compared against.
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

/// Releases every client thread at the same instant.
///
/// Without this the first client would start (and finish) while the last was
/// still opening its socket, so a "16 client" run would spend part of its time
/// with far fewer than 16 clients actually in flight -- and would understate
/// contention exactly where the benchmark is trying to measure it.
class StartGate {
 public:
  explicit StartGate(int participants) : remaining_(participants) {}

  /// Called by each worker once it is connected and ready.
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

std::string pad_key(std::size_t i) {
  const std::string digits = std::to_string(i);
  return "key:" + std::string(12 - digits.size(), '0') + digits;
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
  cachex::SyncCache cache;
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
  // Concurrency scaling. Every configuration performs the same total work, so
  // throughput is directly comparable and the scaling factor is meaningful.
  // ======================================================================
  {
    const std::string value(kValueBytes, 'v');

    // Pre-populate so the GET phase hits. Done before any timing.
    for (std::size_t i = 0; i < kTotalRequests; ++i) {
      cache.set(pad_key(i), value);
    }

    const auto run_scaling = [&](const char* name,
                                 const std::function<std::string(int, std::size_t)>&
                                     make_command) {
      std::cout << "\n" << name << " -- " << kTotalRequests
                << " total requests, split across N clients\n\n"
                << std::left << std::setw(9) << "clients" << std::right
                << std::setw(11) << "per client" << std::setw(13) << "req/sec"
                << std::setw(10) << "scaling" << std::setw(11) << "p50 (us)"
                << std::setw(10) << "p95" << std::setw(10) << "p99"
                << std::setw(11) << "errors" << "\n"
                << std::string(84, '-') << "\n";

      double baseline = 0.0;
      for (const int clients : kClientCounts) {
        const std::size_t per_client =
            kTotalRequests / static_cast<std::size_t>(clients);
        const ConcurrentResult result =
            run_concurrent(server.bound_port(), clients, per_client, make_command);

        const double throughput = bench::ops_per_sec(result.wall, result.requests);
        if (clients == kClientCounts[0]) {
          baseline = throughput;
        }
        std::cout << std::left << std::setw(9) << clients << std::right
                  << std::setw(11) << per_client << std::setw(13) << std::fixed
                  << std::setprecision(0) << throughput << std::setw(9)
                  << std::setprecision(2)
                  << (baseline > 0.0 ? throughput / baseline : 0.0) << "x"
                  << std::setw(11) << std::setprecision(2)
                  << result.latency.p50 / 1000.0 << std::setw(10)
                  << result.latency.p95 / 1000.0 << std::setw(10)
                  << result.latency.p99 / 1000.0 << std::setw(11)
                  << result.failures << "\n";
      }
    };

    std::cout << "\n\nCONCURRENCY SCALING\n"
              << "===================\n"
              << "server: thread-per-connection, one std::mutex around the cache\n"
              << "note:   clients and server share this machine's "
              << std::thread::hardware_concurrency()
              << " hardware threads, so past\n"
              << "        that point they compete with each other for CPU.\n";

    // Control. PING takes no lock and touches no cache data, so whatever
    // ceiling it hits is the transport's, not the cache's. If PING scales like
    // GET, the mutex is not what is limiting either of them.
    run_scaling("PING (control: no lock, no cache access)",
                [](int, std::size_t) { return std::string("PING\n"); });

    run_scaling("GET (all hits)", [&](int, std::size_t i) {
      return "GET " + pad_key(i) + "\n";
    });

    run_scaling("SET", [&value](int client, std::size_t i) {
      // Distinct key space per client, so this measures lock contention rather
      // than clients overwriting each other.
      return "SET c" + std::to_string(client) + ":" + pad_key(i) + " " + value +
             "\n";
    });

    // ====================================================================
    // The same scaling question with the network removed entirely: N threads
    // calling SyncCache::get() directly. This is the mutex on its own, with
    // nothing to hide behind.
    // ====================================================================
    std::cout << "\n\nIN-PROCESS LOCK CONTENTION (no sockets)\n"
              << "N threads calling SyncCache::get() directly, "
              << kTotalRequests << " total calls\n\n"
              << std::left << std::setw(9) << "threads" << std::right
              << std::setw(13) << "ops/sec" << std::setw(10) << "scaling"
              << std::setw(13) << "ns/op" << "\n"
              << std::string(45, '-') << "\n";

    double lock_baseline = 0.0;
    std::uint64_t lock_checksum = 0;
    for (const int threads : kClientCounts) {
      const std::size_t per_thread =
          kTotalRequests / static_cast<std::size_t>(threads);
      StartGate gate(threads);
      std::vector<std::thread> workers;
      workers.reserve(static_cast<std::size_t>(threads));
      std::atomic<std::uint64_t> sink{0};

      for (int w = 0; w < threads; ++w) {
        workers.emplace_back([&, w] {
          gate.arrive_and_wait();
          std::uint64_t local = 0;
          for (std::size_t i = 0; i < per_thread; ++i) {
            if (const auto found = cache.get(pad_key((i * 7 + static_cast<std::size_t>(w)) %
                                                     kTotalRequests))) {
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
      // Printed at the end, so the optimiser cannot delete the gets.
      lock_checksum += sink.load();

      const std::size_t total = per_thread * static_cast<std::size_t>(threads);
      const double throughput = bench::ops_per_sec(elapsed, total);
      if (threads == kClientCounts[0]) {
        lock_baseline = throughput;
      }
      std::cout << std::left << std::setw(9) << threads << std::right
                << std::setw(13) << std::fixed << std::setprecision(0)
                << throughput << std::setw(9) << std::setprecision(2)
                << (lock_baseline > 0.0 ? throughput / lock_baseline : 0.0) << "x"
                << std::setw(13) << std::setprecision(1)
                << bench::avg_ns(elapsed, total) << "\n";
    }

    std::cout << "\n  (checksum " << lock_checksum << ")\n"
              << "\n  scaling = throughput at N / throughput at 1. Perfect would be Nx.\n"
              << "\n  Read the three tables together before blaming anything:\n"
              << "    * PING takes no lock at all. If it plateaus where GET and SET\n"
              << "      plateau, the ceiling over TCP is the transport and the\n"
              << "      scheduler, not the cache mutex.\n"
              << "    * The in-process table is the mutex with nowhere to hide. A\n"
              << "      figure at or below 1.00x there means the lock is pure\n"
              << "      serialisation -- threads take turns, and the extra threads\n"
              << "      only add handoff cost.\n"
              << "    * Latency rising roughly in proportion to client count, while\n"
              << "      throughput is flat, is the signature of a saturated\n"
              << "      resource: the queue is growing, not the service rate.\n";
  }

  server.stop();
  server_thread.join();
  return 0;
}
