#include "cachex/server.hpp"

#include <sys/socket.h>
#include <sys/time.h>

#include <chrono>
#include <string>
#include <thread>

#include "cachex/sync_cache.hpp"
#include "cachex/line_buffer.hpp"
#include "cachex/protocol.hpp"
#include "cachex/socket.hpp"
#include "test_framework.hpp"

namespace {

/// Runs a real Server on a real socket, on an OS-assigned port.
///
/// Port 0 matters: a hard-coded port would collide with a developer's running
/// server, with a second copy of the suite, and with itself if a previous run's
/// socket were still in TIME_WAIT. The kernel hands out a free one instead.
class ServerFixture {
 public:
  ServerFixture() : server_(cache_, options()) {
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
  cachex::SyncCache& cache() { return cache_; }
  const cachex::Server& server() const { return server_; }

 private:
  static cachex::Server::Options options() {
    cachex::Server::Options opts;
    opts.port = 0;         // let the OS choose
    opts.verbose = false;  // keep the test output readable
    return opts;
  }

  cachex::SyncCache cache_;
  cachex::Server server_;
  std::thread thread_;
  bool started_ = false;
};

/// A minimal client. Destroying it closes the connection, which is what lets
/// the single-threaded server move on to the next test's client.
class TestClient {
 public:
  explicit TestClient(std::uint16_t port) {
    std::string error;
    socket_ = cachex::connect_to("127.0.0.1", port, error);
    if (socket_.valid()) {
      // Without a receive timeout, a server bug becomes a hung test suite
      // rather than a failing one. Five seconds is far longer than any
      // legitimate local round trip.
      timeval timeout{};
      timeout.tv_sec = 5;
      ::setsockopt(socket_.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout));
    }
  }

  bool connected() const { return socket_.valid(); }

  bool send_raw(const std::string& data) {
    return cachex::send_all(socket_.get(), data);
  }

  /// Reads one reply line, buffering across recv() calls exactly as the real
  /// client does. Returns nullopt if the server closed or timed out.
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

  /// Sends one command and returns the reply, or "<no reply>" on failure --
  /// a sentinel rather than a crash, so a failing CHECK_EQ shows what happened.
  std::string request(const std::string& command) {
    if (!send_raw(command + "\n")) {
      return "<send failed>";
    }
    return read_reply().value_or("<no reply>");
  }

  void disconnect() { socket_.close(); }

 private:
  cachex::Socket socket_;
  cachex::LineBuffer replies_;
};

}  // namespace

CACHEX_TEST(server_binds_an_ephemeral_port_and_accepts_a_connection) {
  ServerFixture fixture;
  CHECK(fixture.started());
  CHECK(fixture.port() != 0);

  TestClient client(fixture.port());
  CHECK(client.connected());
  CHECK_EQ(client.request("PING"), "+PONG");
}

CACHEX_TEST(set_and_get_round_trip_over_tcp) {
  ServerFixture fixture;
  TestClient client(fixture.port());

  CHECK_EQ(client.request("SET foo bar"), "+OK");
  CHECK_EQ(client.request("GET foo"), "=bar");
}

CACHEX_TEST(get_of_a_missing_key_returns_nil) {
  ServerFixture fixture;
  TestClient client(fixture.port());

  CHECK_EQ(client.request("GET nothing"), "_");
}

CACHEX_TEST(delete_over_tcp_reports_whether_it_removed_anything) {
  ServerFixture fixture;
  TestClient client(fixture.port());

  client.request("SET foo bar");
  CHECK_EQ(client.request("DELETE foo"), ":1");
  CHECK_EQ(client.request("DELETE foo"), ":0");
  CHECK_EQ(client.request("GET foo"), "_");
}

CACHEX_TEST(exists_over_tcp) {
  ServerFixture fixture;
  TestClient client(fixture.port());

  CHECK_EQ(client.request("EXISTS foo"), ":0");
  client.request("SET foo bar");
  CHECK_EQ(client.request("EXISTS foo"), ":1");
}

CACHEX_TEST(ttl_over_tcp_covers_all_three_states) {
  ServerFixture fixture;
  TestClient client(fixture.port());

  CHECK_EQ(client.request("TTL missing"), "_");

  client.request("SET forever v");
  CHECK_EQ(client.request("TTL forever"), "+NOEXPIRE");

  client.request("SET session v 60");
  // Expiry itself is tested at the cache level; here we only need to know the
  // TTL was plumbed through and comes back as a sane number of seconds.
  const std::string reply = client.request("TTL session");
  CHECK(reply == ":60" || reply == ":59");
}

CACHEX_TEST(a_zero_ttl_deletes_over_tcp) {
  ServerFixture fixture;
  TestClient client(fixture.port());

  client.request("SET k v");
  CHECK_EQ(client.request("SET k v 0"), "+OK");
  CHECK_EQ(client.request("EXISTS k"), ":0");
}

// --- error handling --------------------------------------------------------

CACHEX_TEST(an_invalid_command_is_an_error_reply_not_a_disconnect) {
  ServerFixture fixture;
  TestClient client(fixture.port());

  CHECK_EQ(client.request("BOGUS x"), "-ERR unknown command 'BOGUS'");
  // The connection must still be usable: garbage in is not a hang-up.
  CHECK_EQ(client.request("PING"), "+PONG");
}

CACHEX_TEST(malformed_requests_each_get_their_own_error) {
  ServerFixture fixture;
  TestClient client(fixture.port());

  CHECK(client.request("GET").rfind("-ERR wrong number of arguments", 0) == 0);
  CHECK(client.request("SET a b xyz").rfind("-ERR invalid TTL", 0) == 0);
  CHECK(client.request("SET a b -5").rfind("-ERR TTL must not be negative", 0) == 0);
  CHECK_EQ(client.request(""), "-ERR empty command");
  CHECK_EQ(client.request("PING"), "+PONG");  // still alive after all of that
}

CACHEX_TEST(an_over_long_line_is_refused_and_closes_the_connection) {
  ServerFixture fixture;
  TestClient client(fixture.port());

  // No newline anywhere: the server must not buffer this indefinitely.
  const std::string flood(cachex::kMaxLineBytes + 1024, 'x');
  client.send_raw(flood);

  const std::optional<std::string> reply = client.read_reply();
  CHECK(reply.has_value());
  CHECK(reply.value_or("").rfind("-ERR line too long", 0) == 0);

  // And the connection really is closed afterwards.
  CHECK(!client.read_reply().has_value());
}

// --- framing over a real socket --------------------------------------------

CACHEX_TEST(multiple_commands_on_one_connection) {
  ServerFixture fixture;
  TestClient client(fixture.port());

  for (int i = 0; i < 50; ++i) {
    const std::string key = "k" + std::to_string(i);
    CHECK_EQ(client.request("SET " + key + " v" + std::to_string(i)), "+OK");
  }
  bool all_correct = true;
  for (int i = 0; i < 50; ++i) {
    if (client.request("GET k" + std::to_string(i)) != "=v" + std::to_string(i)) {
      all_correct = false;
    }
  }
  CHECK(all_correct);
  CHECK_EQ(fixture.cache().size(), 50u);
}

// Several commands in a single write. The server must answer all of them from
// what could easily be one recv(), not just the first.
CACHEX_TEST(several_commands_sent_in_one_write_all_get_answered) {
  ServerFixture fixture;
  TestClient client(fixture.port());

  CHECK(client.send_raw("SET a 1\nSET b 2\nGET a\nGET b\nPING\n"));

  CHECK_EQ(client.read_reply().value_or("x"), "+OK");
  CHECK_EQ(client.read_reply().value_or("x"), "+OK");
  CHECK_EQ(client.read_reply().value_or("x"), "=1");
  CHECK_EQ(client.read_reply().value_or("x"), "=2");
  CHECK_EQ(client.read_reply().value_or("x"), "+PONG");
}

// The mirror image: one command split across several writes, with a pause so
// the bytes really do arrive in separate packets.
CACHEX_TEST(a_command_split_across_writes_is_reassembled_by_the_server) {
  ServerFixture fixture;
  TestClient client(fixture.port());

  CHECK(client.send_raw("SET spl"));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(client.send_raw("it value"));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(client.send_raw("\n"));

  CHECK_EQ(client.read_reply().value_or("x"), "+OK");
  CHECK_EQ(client.request("GET split"), "=value");
}

CACHEX_TEST(crlf_clients_work_over_tcp) {
  ServerFixture fixture;
  TestClient client(fixture.port());

  CHECK(client.send_raw("SET foo bar\r\nGET foo\r\n"));
  CHECK_EQ(client.read_reply().value_or("x"), "+OK");
  CHECK_EQ(client.read_reply().value_or("x"), "=bar");
}

// --- connection lifecycle --------------------------------------------------

CACHEX_TEST(quit_is_acknowledged_before_the_connection_closes) {
  ServerFixture fixture;
  TestClient client(fixture.port());

  CHECK_EQ(client.request("QUIT"), "+BYE");
  // The +BYE arrives first, then EOF -- the client should never have to guess.
  CHECK(!client.read_reply().has_value());
}

CACHEX_TEST(a_client_can_disconnect_and_another_can_reconnect) {
  ServerFixture fixture;

  {
    TestClient first(fixture.port());
    CHECK_EQ(first.request("SET shared value"), "+OK");
  }  // first disconnects here without saying QUIT

  TestClient second(fixture.port());
  CHECK(second.connected());
  // The cache outlives the connection: state belongs to the server, not the
  // socket.
  CHECK_EQ(second.request("GET shared"), "=value");
}

CACHEX_TEST(an_abrupt_disconnect_does_not_take_the_server_down) {
  ServerFixture fixture;

  for (int i = 0; i < 5; ++i) {
    TestClient client(fixture.port());
    CHECK(client.connected());
    client.send_raw("SET k" + std::to_string(i) + " v\n");
    // Close without reading the reply and without QUIT -- the server is writing
    // into a socket the peer has already abandoned. It must survive that
    // (SIGPIPE is suppressed) rather than dying.
    client.disconnect();
  }

  TestClient survivor(fixture.port());
  CHECK(survivor.connected());
  CHECK_EQ(survivor.request("PING"), "+PONG");
}

CACHEX_TEST(connections_are_served_one_after_another) {
  ServerFixture fixture;

  for (int i = 0; i < 3; ++i) {
    TestClient client(fixture.port());
    CHECK_EQ(client.request("PING"), "+PONG");
  }
  CHECK_EQ(fixture.server().connections_served(), 3u);
}

CACHEX_TEST(the_server_state_is_the_cache_the_caller_owns) {
  ServerFixture fixture;
  fixture.cache().set("preloaded", "yes");

  TestClient client(fixture.port());
  CHECK_EQ(client.request("GET preloaded"), "=yes");

  client.request("SET from_network 1");
  CHECK(fixture.cache().contains("from_network"));
}
