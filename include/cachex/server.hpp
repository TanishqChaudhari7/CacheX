#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "cachex/cache.hpp"
#include "cachex/socket.hpp"

namespace cachex {

/// A single-threaded TCP front end for a Cache.
///
/// **One client at a time.** accept() returns a connection, it is served to
/// completion, and only then does the loop accept the next one. A second client
/// waits in the kernel's backlog queue. That is a real limitation, and making it
/// concurrent is Stage 7 -- the cache is not thread-safe, so serving two clients
/// in parallel today would be a data race, not a feature.
class Server {
 public:
  struct Options {
    std::string host = "127.0.0.1";
    /// 0 asks the OS for any free port; bound_port() then reports which.
    /// Tests rely on this so they never collide with each other or with a real
    /// server on a fixed port.
    std::uint16_t port = 6379;
    /// How many completed connections the kernel may queue while we are busy
    /// serving someone. Beyond this, new connections are refused.
    int backlog = 128;
    bool verbose = true;
  };

  Server(Cache& cache, Options options);

  /// socket() + bind() + listen(). Returns false on failure with `error` set.
  /// Separate from run() so that a caller -- notably a test -- can learn the
  /// bound port and connect to it before the accept loop starts.
  bool start(std::string& error);

  /// The port actually bound. Meaningful only after a successful start().
  std::uint16_t bound_port() const noexcept { return bound_port_; }

  /// Accepts and serves connections until stop() is called. Blocking.
  void run();

  /// Asks run() to return. Safe to call from another thread; that is the only
  /// reason anything here is atomic. The cache remains single-threaded --
  /// exactly one thread ever touches it.
  void stop() noexcept { stop_requested_.store(true); }

  std::size_t connections_served() const noexcept { return connections_served_; }

 private:
  Cache& cache_;
  Options options_;
  Socket listener_;
  std::uint16_t bound_port_ = 0;
  std::atomic<bool> stop_requested_{false};
  std::size_t connections_served_ = 0;
};

}  // namespace cachex
