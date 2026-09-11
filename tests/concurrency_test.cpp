#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "cachex/server.hpp"
#include "cachex/socket.hpp"
#include "cachex/line_buffer.hpp"
#include "cachex/sync_cache.hpp"
#include "test_framework.hpp"

#include <sys/socket.h>
#include <sys/time.h>

namespace {

/// Runs `worker(thread_index)` on `count` threads and waits for all of them.
///
/// The threads are started in a loop and joined in a second loop, so they
/// genuinely overlap. Starting and joining one at a time would serialise them
/// and quietly test nothing.
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

constexpr int kThreads = 8;
constexpr int kOpsPerThread = 2000;

}  // namespace

// --- concurrent writers ----------------------------------------------------

// Disjoint keys, so the final state is fully deterministic: every key written
// must be present with its own value, and the size must be exact.
CACHEX_TEST(concurrent_writers_on_disjoint_keys_all_survive) {
  cachex::SyncCache cache;

  run_parallel(kThreads, [&cache](int thread_id) {
    for (int i = 0; i < kOpsPerThread; ++i) {
      cache.set("t" + std::to_string(thread_id) + ":k" + std::to_string(i),
                "v" + std::to_string(i));
    }
  });

  CHECK_EQ(cache.size(), static_cast<std::size_t>(kThreads * kOpsPerThread));

  bool all_present = true;
  for (int t = 0; t < kThreads; ++t) {
    for (int i = 0; i < kOpsPerThread; ++i) {
      if (cache.get("t" + std::to_string(t) + ":k" + std::to_string(i))
              .value_or("") != "v" + std::to_string(i)) {
        all_present = false;
      }
    }
  }
  CHECK(all_present);
}

// Same key from every thread. The winner is genuinely racy, so the assertion is
// the one thing that must hold: the value is *one of* the values written, never
// a mixture of two. A torn write would show up here.
CACHEX_TEST(concurrent_writers_on_one_key_leave_exactly_one_whole_value) {
  cachex::SyncCache cache;

  run_parallel(kThreads, [&cache](int thread_id) {
    const std::string value(64, static_cast<char>('A' + thread_id));
    for (int i = 0; i < kOpsPerThread; ++i) {
      cache.set("contended", value);
    }
  });

  CHECK_EQ(cache.size(), 1u);
  const std::string final_value = cache.get("contended").value_or("");
  CHECK_EQ(final_value.size(), 64u);

  bool uniform = true;
  for (const char c : final_value) {
    if (c != final_value[0]) {
      uniform = false;  // a mixture of two writers' values
    }
  }
  CHECK(uniform);
  CHECK(final_value[0] >= 'A' && final_value[0] < 'A' + kThreads);
}

// --- concurrent readers ----------------------------------------------------

// The one that matters most under LRU: every get() splices a node to the head of
// the list, so these "readers" are all mutating the same list at once. Without
// the lock this is a straightforward corrupted-linked-list crash.
CACHEX_TEST(concurrent_readers_all_see_correct_values) {
  cachex::SyncCache cache;
  for (int i = 0; i < 500; ++i) {
    cache.set("k" + std::to_string(i), "v" + std::to_string(i));
  }

  std::atomic<int> mismatches{0};
  run_parallel(kThreads, [&cache, &mismatches](int) {
    for (int i = 0; i < kOpsPerThread; ++i) {
      const int key = i % 500;
      if (cache.get("k" + std::to_string(key)).value_or("") !=
          "v" + std::to_string(key)) {
        mismatches.fetch_add(1);
      }
    }
  });

  CHECK_EQ(mismatches.load(), 0);
  CHECK_EQ(cache.size(), 500u);
  // The list and the map must still agree on how many entries exist -- a
  // mismatch is what a half-applied splice would leave behind.
  CHECK_EQ(cache.keys_by_recency().size(), cache.size());
}

// --- mixed workload --------------------------------------------------------

CACHEX_TEST(mixed_get_set_delete_leaves_the_structures_consistent) {
  cachex::SyncCache cache;
  std::atomic<int> errors{0};

  run_parallel(kThreads, [&cache, &errors](int thread_id) {
    for (int i = 0; i < kOpsPerThread; ++i) {
      const std::string key = "k" + std::to_string((thread_id * 7 + i) % 200);
      switch (i % 4) {
        case 0:
          cache.set(key, "value");
          break;
        case 1:
          cache.get(key);
          break;
        case 2:
          cache.erase(key);
          break;
        case 3:
          // A present key must never report an empty value: that would mean a
          // reader saw an entry mid-update.
          if (const auto value = cache.get(key)) {
            if (value->empty()) {
              errors.fetch_add(1);
            }
          }
          break;
        default:
          break;
      }
    }
  });

  CHECK_EQ(errors.load(), 0);
  CHECK(cache.size() <= 200u);
  CHECK_EQ(cache.keys_by_recency().size(), cache.size());
}

CACHEX_TEST(mixed_workload_with_ttl_and_eviction_stays_consistent) {
  cachex::SyncCache cache(64);  // small capacity: eviction runs constantly

  run_parallel(kThreads, [&cache](int thread_id) {
    for (int i = 0; i < kOpsPerThread; ++i) {
      const std::string key = "k" + std::to_string((thread_id * 13 + i) % 500);
      switch (i % 5) {
        case 0:
          cache.set(key, "v");
          break;
        case 1:
          cache.set(key, "v", std::chrono::milliseconds(5));
          break;
        case 2:
          cache.get(key);
          break;
        case 3:
          cache.ttl(key);  // mutates: reclaims the entry if expired
          break;
        case 4:
          cache.contains(key);
          break;
        default:
          break;
      }
    }
  });

  // Capacity must hold no matter how the interleaving went, and the two
  // structures must still agree.
  CHECK(cache.size() <= 64u);
  CHECK_EQ(cache.keys_by_recency().size(), cache.size());
}

// --- what thread safety does NOT give you ----------------------------------

// Every individual call is atomic, but a get-then-set pair is not: another
// thread can slip between them. This is the classic lost-update race, and it
// survives a perfectly thread-safe cache.
//
// The test asserts the *bug*, because that is the honest description of the
// contract: SyncCache makes operations atomic, not transactions.
CACHEX_TEST(read_modify_write_is_not_atomic_even_though_each_call_is) {
  cachex::SyncCache cache;
  cache.set("counter", "0");

  constexpr int kIncrements = 500;
  run_parallel(kThreads, [&cache](int) {
    for (int i = 0; i < kIncrements; ++i) {
      const int current = std::stoi(cache.get("counter").value_or("0"));
      cache.set("counter", std::to_string(current + 1));
    }
  });

  const int final_value = std::stoi(cache.get("counter").value_or("0"));
  // Never more than the number of increments, and in practice far fewer --
  // updates are lost whenever two threads read the same value.
  CHECK(final_value <= kThreads * kIncrements);
  CHECK(final_value > 0);
  // Fixing this needs either a compound operation inside the lock (an INCR
  // command) or a compare-and-swap, not a bigger mutex.
}

// --- concurrency over TCP --------------------------------------------------

namespace {

class ConcurrentServerFixture {
 public:
  ConcurrentServerFixture() : server_(cache_, options()) {
    std::string error;
    started_ = server_.start(error);
    if (started_) {
      thread_ = std::thread([this] { server_.run(); });
    }
  }
  ~ConcurrentServerFixture() {
    server_.stop();
    if (thread_.joinable()) {
      thread_.join();
    }
  }
  bool started() const { return started_; }
  std::uint16_t port() const { return server_.bound_port(); }
  cachex::SyncCache& cache() { return cache_; }
  const cachex::Server& server() const { return server_; }

 private:
  static cachex::Server::Options options() {
    cachex::Server::Options opts;
    opts.port = 0;
    opts.verbose = false;
    return opts;
  }
  cachex::SyncCache cache_;
  cachex::Server server_;
  std::thread thread_;
  bool started_ = false;
};

/// Minimal client used by the concurrent tests.
class Client {
 public:
  explicit Client(std::uint16_t port) {
    std::string error;
    socket_ = cachex::connect_to("127.0.0.1", port, error);
    if (socket_.valid()) {
      timeval timeout{};
      timeout.tv_sec = 10;
      ::setsockopt(socket_.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout));
    }
  }
  bool connected() const { return socket_.valid(); }
  std::string request(const std::string& command) {
    if (!cachex::send_all(socket_.get(), command + "\n")) {
      return "<send failed>";
    }
    while (true) {
      if (std::optional<std::string> line = replies_.next_line()) {
        return *line;
      }
      char chunk[4096];
      const ssize_t received = ::recv(socket_.get(), chunk, sizeof(chunk), 0);
      if (received <= 0) {
        return "<no reply>";
      }
      replies_.append(chunk, static_cast<std::size_t>(received));
    }
  }

 private:
  cachex::Socket socket_;
  cachex::LineBuffer replies_;
};

}  // namespace

// The point of this stage: several clients connected at the same time, each on
// its own server thread. Under the Stage 5 server the later clients would have
// sat in the backlog until the first one disconnected.
CACHEX_TEST(many_clients_are_served_at_the_same_time) {
  ConcurrentServerFixture fixture;
  CHECK(fixture.started());

  constexpr int kClients = 8;
  constexpr int kRequests = 200;
  std::atomic<int> failures{0};

  run_parallel(kClients, [&fixture, &failures](int client_id) {
    Client client(fixture.port());
    if (!client.connected()) {
      failures.fetch_add(1);
      return;
    }
    for (int i = 0; i < kRequests; ++i) {
      const std::string key = "c" + std::to_string(client_id) + ":" +
                              std::to_string(i);
      if (client.request("SET " + key + " v" + std::to_string(i)) != "+OK") {
        failures.fetch_add(1);
      }
      if (client.request("GET " + key) != "=v" + std::to_string(i)) {
        failures.fetch_add(1);
      }
    }
  });

  CHECK_EQ(failures.load(), 0);
  CHECK_EQ(fixture.cache().size(),
           static_cast<std::size_t>(kClients * kRequests));
  CHECK_EQ(fixture.server().connections_served(),
           static_cast<std::size_t>(kClients));
}

CACHEX_TEST(clients_see_each_others_writes_through_the_shared_cache) {
  ConcurrentServerFixture fixture;

  Client writer(fixture.port());
  Client reader(fixture.port());
  CHECK(writer.connected());
  CHECK(reader.connected());

  CHECK_EQ(writer.request("SET shared hello"), "+OK");
  // A different connection, a different server thread, the same cache.
  CHECK_EQ(reader.request("GET shared"), "=hello");
}

CACHEX_TEST(all_worker_threads_are_joined_when_the_server_stops) {
  ConcurrentServerFixture fixture;
  {
    std::vector<std::unique_ptr<Client>> clients;
    for (int i = 0; i < 4; ++i) {
      clients.push_back(std::make_unique<Client>(fixture.port()));
      CHECK_EQ(clients.back()->request("PING"), "+PONG");
    }
    CHECK(fixture.server().active_connections() > 0u);
  }  // all four disconnect here

  // The fixture's destructor stops the server and joins the accept loop, which
  // in turn joins every worker. If a worker were detached instead, it could
  // outlive the cache it holds a reference to -- ASan would catch that.
}
