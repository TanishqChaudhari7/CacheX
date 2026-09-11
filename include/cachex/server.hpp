#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cachex/sync_cache.hpp"
#include "cachex/socket.hpp"

namespace cachex {

/// A thread-per-connection TCP front end for a SyncCache.
///
/// The accept loop runs on the calling thread and does nothing but accept: each
/// accepted connection is handed to its own worker thread, so N clients are
/// served by N threads in parallel. They all share one SyncCache, which
/// serialises them on its mutex.
///
/// Thread-per-connection is the simplest model that actually works, which makes
/// it the right baseline -- but it does not scale: each thread costs a stack
/// (~512 KiB-8 MiB of address space) and a scheduler slot, so `max_connections`
/// caps it. An event loop serving thousands of connections from a handful of
/// threads is the alternative, and it is a much larger change.
class Server {
 public:
  struct Options {
    std::string host = "127.0.0.1";
    /// 0 asks the OS for any free port; bound_port() then reports which.
    /// Tests rely on this so they never collide with each other or with a real
    /// server on a fixed port.
    std::uint16_t port = 6379;
    /// How many completed connections the kernel may queue while we are busy
    /// accepting. Beyond this, new connections are refused.
    int backlog = 128;
    /// Hard cap on worker threads. Thread-per-connection means an unbounded
    /// client count would be an unbounded thread count; a client arriving past
    /// this limit is told so and disconnected rather than exhausting memory.
    std::size_t max_connections = 256;
    bool verbose = true;
  };

  Server(SyncCache& cache, Options options);

  /// socket() + bind() + listen(). Returns false on failure with `error` set.
  /// Separate from run() so that a caller -- notably a test -- can learn the
  /// bound port and connect to it before the accept loop starts.
  bool start(std::string& error);

  /// The port actually bound. Meaningful only after a successful start().
  std::uint16_t bound_port() const noexcept { return bound_port_; }

  /// Accepts and serves connections until stop() is called. Blocking.
  void run();

  /// Asks run() to stop accepting. Safe to call from another thread.
  /// run() then joins every worker before returning, so no thread outlives the
  /// Server or the cache it references.
  void stop() noexcept { stop_requested_.store(true); }

  /// Connections accepted over the server's lifetime.
  std::size_t connections_served() const noexcept {
    return connections_served_.load();
  }
  /// Connections currently being served, i.e. live worker threads.
  std::size_t active_connections() const noexcept {
    return active_connections_.load();
  }

 private:
  /// One worker thread plus the flag it sets on its way out, so the accept loop
  /// can tell which threads are finished and join them. std::thread cannot be
  /// asked "are you done?", so the worker has to say so itself.
  struct Worker {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> finished;
  };

  void spawn_worker(Socket client);
  /// Joins and removes workers that have finished, so the vector does not grow
  /// without bound on a long-running server.
  void reap_finished_workers();
  /// Joins every remaining worker. Called once, after the accept loop exits.
  void join_all_workers();

  SyncCache& cache_;
  Options options_;
  Socket listener_;
  std::uint16_t bound_port_ = 0;
  std::atomic<bool> stop_requested_{false};

  // Written by worker threads as well as the accept loop, so both are atomic.
  std::atomic<std::size_t> connections_served_{0};
  std::atomic<std::size_t> active_connections_{0};

  // Guards workers_ only. A much smaller critical section than the cache's
  // mutex, and never held while a connection is being served.
  std::mutex workers_mutex_;
  std::vector<Worker> workers_;
};

}  // namespace cachex
