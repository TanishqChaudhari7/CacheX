#pragma once

// Helpers shared by the tests that need threads or a real server.

#include <sys/socket.h>
#include <sys/time.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "cachex/line_buffer.hpp"
#include "cachex/persistence.hpp"
#include "cachex/server.hpp"
#include "cachex/sharded_cache.hpp"
#include "cachex/socket.hpp"

namespace cachex::testing {

/// Runs `worker(thread_index)` on `count` threads and waits for all of them.
///
/// The threads are started in one loop and joined in a second, so they really
/// overlap. Starting and joining one at a time would serialise them and quietly
/// test nothing.
template <typename F>
void run_parallel(int count, F worker) {
  std::vector<std::thread> threads;
  threads.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    threads.emplace_back([&worker, i] { worker(i); });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
}

/// A real Server on a real socket, on an OS-assigned port, running on its own
/// thread for the lifetime of the fixture.
///
/// Port 0 matters: a fixed port would collide with a developer's running server,
/// with a second copy of the suite, or with a socket still in TIME_WAIT.
class ServerFixture {
 public:
  explicit ServerFixture(std::size_t max_connections = 256,
                         PersistenceManager* persistence = nullptr)
      : server_(cache_, options(max_connections), persistence) {
    std::string error;
    started_ = server_.start(error);
    if (started_) {
      thread_ = std::thread([this] { server_.run(); });
    }
  }

  ~ServerFixture() {
    server_.stop();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  ServerFixture(const ServerFixture&) = delete;
  ServerFixture& operator=(const ServerFixture&) = delete;

  bool started() const { return started_; }
  std::uint16_t port() const { return server_.bound_port(); }
  ShardedCache& cache() { return cache_; }
  const Server& server() const { return server_; }

 private:
  static Server::Options options(std::size_t max_connections) {
    Server::Options opts;
    opts.port = 0;
    opts.verbose = false;
    opts.max_connections = max_connections;
    return opts;
  }

  ShardedCache cache_{1};
  Server server_;
  std::thread thread_;
  bool started_ = false;
};

/// A minimal protocol client. Destroying it closes the connection.
class TestClient {
 public:
  explicit TestClient(std::uint16_t port) {
    std::string error;
    socket_ = connect_to("127.0.0.1", port, error);
    if (socket_.valid()) {
      // Without a receive timeout a server bug becomes a hung suite rather than
      // a failing test.
      timeval timeout{};
      timeout.tv_sec = 10;
      ::setsockopt(socket_.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    }
  }

  bool connected() const { return socket_.valid(); }

  bool send_raw(const std::string& data) { return send_all(socket_.get(), data); }

  /// One reply line, buffered across recv() calls exactly as the real client
  /// does. nullopt if the server closed the connection or the read timed out.
  std::optional<std::string> read_reply() {
    while (true) {
      if (std::optional<std::string> line = replies_.next_line()) {
        return line;
      }
      char chunk[4096];
      const ssize_t received = ::recv(socket_.get(), chunk, sizeof(chunk), 0);
      if (received <= 0) {
        return std::nullopt;
      }
      replies_.append(chunk, static_cast<std::size_t>(received));
    }
  }

  /// Sends one command and returns its reply, or a readable sentinel on failure
  /// so a failing CHECK_EQ shows what happened.
  std::string request(const std::string& command) {
    if (!send_raw(command + "\n")) {
      return "<send failed>";
    }
    return read_reply().value_or("<no reply>");
  }

  void disconnect() { socket_.close(); }

 private:
  Socket socket_;
  LineBuffer replies_;
};

}  // namespace cachex::testing
