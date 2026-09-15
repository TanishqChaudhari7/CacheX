#include "cachex/server.hpp"

#include <sys/socket.h>
#include <sys/time.h>

#include <chrono>
#include <string>
#include <thread>

#include "cachex/sharded_cache.hpp"
#include "cachex/line_buffer.hpp"
#include "cachex/protocol.hpp"
#include "cachex/socket.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using cachex::testing::ServerFixture;
using cachex::testing::TestClient;

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

CACHEX_TEST(a_client_beyond_the_connection_limit_is_told_and_disconnected) {
  ServerFixture fixture(/*max_connections=*/1);
  CHECK(fixture.started());

  TestClient first(fixture.port());
  CHECK_EQ(first.request("PING"), "+PONG");  // the one allowed connection

  TestClient second(fixture.port());
  CHECK_EQ(second.read_reply().value_or("<none>"), "-ERR server at connection limit (1)");
  CHECK(!second.read_reply().has_value());  // and then closed

  CHECK_EQ(first.request("PING"), "+PONG");  // the admitted client is unaffected

  // Once the first client leaves, its slot is released for someone else. The
  // worker notices the disconnect asynchronously, so allow it a moment.
  first.disconnect();
  bool admitted = false;
  for (int attempt = 0; attempt < 100 && !admitted; ++attempt) {
    TestClient retry(fixture.port());
    admitted = retry.request("PING") == "+PONG";
    if (!admitted) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  CHECK(admitted);
}

