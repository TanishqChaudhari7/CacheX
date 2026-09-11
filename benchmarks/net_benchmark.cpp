#include <sys/socket.h>

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "bench_util.hpp"
#include "cachex/cache.hpp"
#include "cachex/line_buffer.hpp"
#include "cachex/server.hpp"
#include "cachex/socket.hpp"

namespace {

using bench::Clock;
using bench::Nanos;

constexpr std::size_t kRequests = 20000;  // per phase
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
  cachex::Cache cache;
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
  }  // the client disconnects here, freeing the server's accept loop

  server.stop();
  server_thread.join();
  return 0;
}
