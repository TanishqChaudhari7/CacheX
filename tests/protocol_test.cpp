#include "cachex/protocol.hpp"

#include <unistd.h>

#include <cstdio>

#include <string>

#include "cachex/sharded_cache.hpp"
#include "cachex/command_handler.hpp"
#include "cachex/persistence.hpp"
#include "cachex/socket.hpp"
#include "test_framework.hpp"

namespace {

std::string type_name(cachex::CommandType type) {
  return cachex::command_name(type);
}

}  // namespace

// --- valid commands --------------------------------------------------------

CACHEX_TEST(parses_set_without_ttl) {
  const auto result = cachex::parse_command("SET foo bar");
  CHECK(result.ok);
  CHECK_EQ(type_name(result.command.type), "SET");
  CHECK_EQ(result.command.key, "foo");
  CHECK_EQ(result.command.value, "bar");
  CHECK(!result.command.ttl.has_value());
}

CACHEX_TEST(parses_set_with_ttl) {
  const auto result = cachex::parse_command("SET foo bar 10");
  CHECK(result.ok);
  CHECK(result.command.ttl.has_value());
  CHECK_EQ(result.command.ttl.value_or(std::chrono::seconds(0)).count(), 10);
}

CACHEX_TEST(parses_the_key_only_commands) {
  CHECK_EQ(type_name(cachex::parse_command("GET foo").command.type), "GET");
  CHECK_EQ(type_name(cachex::parse_command("DELETE foo").command.type), "DELETE");
  CHECK_EQ(type_name(cachex::parse_command("EXISTS foo").command.type), "EXISTS");
  CHECK_EQ(type_name(cachex::parse_command("TTL foo").command.type), "TTL");
  CHECK(cachex::parse_command("GET foo").ok);
}

CACHEX_TEST(del_is_accepted_as_an_alias_for_delete) {
  const auto result = cachex::parse_command("DEL foo");
  CHECK(result.ok);
  CHECK_EQ(type_name(result.command.type), "DELETE");
}

CACHEX_TEST(parses_the_argumentless_commands) {
  CHECK(cachex::parse_command("PING").ok);
  CHECK(cachex::parse_command("QUIT").ok);
  CHECK_EQ(type_name(cachex::parse_command("PING").command.type), "PING");
  CHECK_EQ(type_name(cachex::parse_command("QUIT").command.type), "QUIT");
}

CACHEX_TEST(command_names_are_case_insensitive) {
  CHECK(cachex::parse_command("get foo").ok);
  CHECK(cachex::parse_command("GeT foo").ok);
  CHECK_EQ(type_name(cachex::parse_command("set a b").command.type), "SET");
  // ...but keys and values are not.
  CHECK_EQ(cachex::parse_command("SET Key Value").command.key, "Key");
  CHECK_EQ(cachex::parse_command("SET Key Value").command.value, "Value");
}

CACHEX_TEST(runs_of_separators_collapse) {
  const auto result = cachex::parse_command("SET    foo \t  bar");
  CHECK(result.ok);
  CHECK_EQ(result.command.key, "foo");
  CHECK_EQ(result.command.value, "bar");
}

CACHEX_TEST(leading_and_trailing_whitespace_is_ignored) {
  const auto result = cachex::parse_command("   GET foo   ");
  CHECK(result.ok);
  CHECK_EQ(result.command.key, "foo");
}

// --- invalid commands ------------------------------------------------------

CACHEX_TEST(an_empty_line_is_an_error_not_a_silent_no_op) {
  const auto result = cachex::parse_command("");
  CHECK(!result.ok);
  CHECK_EQ(result.error, "empty command");
  CHECK(!cachex::parse_command("   ").ok);
}

CACHEX_TEST(unknown_commands_are_reported_with_the_offending_token) {
  const auto result = cachex::parse_command("BOGUS x");
  CHECK(!result.ok);
  CHECK(result.error.find("unknown command") != std::string::npos);
  CHECK(result.error.find("'BOGUS'") != std::string::npos);
}

CACHEX_TEST(wrong_argument_counts_are_rejected) {
  CHECK(!cachex::parse_command("GET").ok);
  CHECK(!cachex::parse_command("GET a b").ok);
  CHECK(!cachex::parse_command("SET").ok);
  CHECK(!cachex::parse_command("SET a").ok);
  CHECK(!cachex::parse_command("SET a b c d").ok);
  CHECK(!cachex::parse_command("PING extra").ok);
  CHECK(!cachex::parse_command("QUIT now").ok);
  CHECK(!cachex::parse_command("EXISTS").ok);
  CHECK(!cachex::parse_command("TTL").ok);
}

CACHEX_TEST(the_argument_count_error_shows_the_expected_form) {
  const auto result = cachex::parse_command("GET");
  CHECK(!result.ok);
  CHECK(result.error.find("GET key") != std::string::npos);
}

CACHEX_TEST(a_non_numeric_ttl_is_rejected) {
  const auto result = cachex::parse_command("SET a b abc");
  CHECK(!result.ok);
  CHECK(result.error.find("invalid TTL") != std::string::npos);
}

// atoi would return 12 here and silently drop the rest. from_chars requires the
// whole token to be consumed, so partial garbage is caught.
CACHEX_TEST(a_partially_numeric_ttl_is_rejected) {
  CHECK(!cachex::parse_command("SET a b 12abc").ok);
  CHECK(!cachex::parse_command("SET a b 1.5").ok);
  CHECK(!cachex::parse_command("SET a b 0x10").ok);
  CHECK(!cachex::parse_command("SET a b +").ok);
}

CACHEX_TEST(a_negative_ttl_is_rejected_rather_than_treated_as_a_delete) {
  const auto result = cachex::parse_command("SET a b -1");
  CHECK(!result.ok);
  CHECK(result.error.find("negative") != std::string::npos);
}

CACHEX_TEST(a_zero_ttl_parses_and_means_delete) {
  // Accepted by the parser; the cache's ttl <= 0 rule turns it into a delete.
  const auto result = cachex::parse_command("SET a b 0");
  CHECK(result.ok);
  CHECK_EQ(result.command.ttl.value_or(std::chrono::seconds(-1)).count(), 0);
}

CACHEX_TEST(an_absurdly_large_ttl_is_rejected) {
  const auto result = cachex::parse_command("SET a b 99999999999");
  CHECK(!result.ok);
  CHECK(result.error.find("too large") != std::string::npos);
}

CACHEX_TEST(a_ttl_that_overflows_long_long_is_rejected) {
  // from_chars reports result_out_of_range rather than wrapping around.
  CHECK(!cachex::parse_command("SET a b 999999999999999999999999").ok);
}

CACHEX_TEST(a_huge_junk_token_is_not_echoed_back_in_full) {
  const std::string huge(50000, 'Z');
  const auto result = cachex::parse_command(huge + " x");
  CHECK(!result.ok);
  // Truncated so an error reply cannot be used to reflect bulk data.
  CHECK(result.error.size() < 100u);
  CHECK(result.error.find("...") != std::string::npos);
}

// --- reply formatting ------------------------------------------------------

CACHEX_TEST(replies_are_single_lines_with_a_type_tag) {
  CHECK_EQ(cachex::reply_ok(), "+OK\n");
  CHECK_EQ(cachex::reply_pong(), "+PONG\n");
  CHECK_EQ(cachex::reply_bye(), "+BYE\n");
  CHECK_EQ(cachex::reply_no_expiry(), "+NOEXPIRE\n");
  CHECK_EQ(cachex::reply_nil(), "_\n");
  CHECK_EQ(cachex::reply_value("bar"), "=bar\n");
  CHECK_EQ(cachex::reply_integer(0), ":0\n");
  CHECK_EQ(cachex::reply_integer(-1), ":-1\n");
  CHECK_EQ(cachex::reply_error("boom"), "-ERR boom\n");
}

CACHEX_TEST(an_empty_value_is_distinguishable_from_nil) {
  // "=\n" is an empty string; "_\n" is a missing key. The type tag is what
  // keeps these apart -- a bare line protocol could not tell them apart.
  CHECK_EQ(cachex::reply_value(""), "=\n");
  CHECK(cachex::reply_value("") != cachex::reply_nil());
}

// --- command execution (no sockets involved) -------------------------------

CACHEX_TEST(execute_runs_a_whole_request_response_cycle_without_a_socket) {
  cachex::ShardedCache cache{1};
  const auto run = [&cache](const std::string& line) {
    const auto parsed = cachex::parse_command(line);
    return parsed.ok ? cachex::execute(cache, parsed.command)
                     : cachex::reply_error(parsed.error);
  };

  CHECK_EQ(run("PING"), "+PONG\n");
  CHECK_EQ(run("GET foo"), "_\n");
  CHECK_EQ(run("EXISTS foo"), ":0\n");
  CHECK_EQ(run("SET foo bar"), "+OK\n");
  CHECK_EQ(run("GET foo"), "=bar\n");
  CHECK_EQ(run("EXISTS foo"), ":1\n");
  CHECK_EQ(run("TTL foo"), "+NOEXPIRE\n");
  CHECK_EQ(run("DELETE foo"), ":1\n");
  CHECK_EQ(run("DELETE foo"), ":0\n");
  CHECK_EQ(run("GET foo"), "_\n");
  CHECK_EQ(run("BOGUS"), "-ERR unknown command 'BOGUS'\n");
  CHECK_EQ(run("QUIT"), "+BYE\n");
}

CACHEX_TEST(execute_reports_ttl_in_seconds_rounded_up) {
  cachex::ShardedCache cache{1};
  cache.set("k", "v", std::chrono::milliseconds(900));

  // 900 ms left must not report ":0" -- the key is still readable.
  cachex::Command command;
  command.type = cachex::CommandType::Ttl;
  command.key = "k";
  CHECK_EQ(cachex::execute(cache, command), ":1\n");
}

CACHEX_TEST(execute_applies_a_ttl_of_zero_as_a_delete) {
  cachex::ShardedCache cache{1};
  cache.set("k", "v");

  const auto parsed = cachex::parse_command("SET k v 0");
  CHECK(parsed.ok);
  CHECK_EQ(cachex::execute(cache, parsed.command), "+OK\n");
  CHECK(!cache.contains("k"));
}

CACHEX_TEST(a_plain_set_clears_an_existing_ttl_over_the_wire_too) {
  cachex::ShardedCache cache{1};
  const auto run = [&cache](const std::string& line) {
    const auto parsed = cachex::parse_command(line);
    return cachex::execute(cache, parsed.command);
  };

  run("SET k v 60");
  CHECK_EQ(run("TTL k"), ":60\n");
  run("SET k v");
  CHECK_EQ(run("TTL k"), "+NOEXPIRE\n");
}

// --- SAVE / LOAD through the command handler -------------------------------

CACHEX_TEST(save_and_load_report_entry_counts) {
  const std::string path = "/tmp/cachex_protocol_save_" + std::to_string(::getpid()) + ".cxs";
  std::remove(path.c_str());
  cachex::PersistenceManager persistence(path);

  cachex::ShardedCache source{2};
  source.set("a", "1");
  source.set("b", "2");
  const auto save = cachex::parse_command("SAVE");
  CHECK(save.ok);
  CHECK_EQ(cachex::execute(source, save.command, &persistence), ":2\n");

  cachex::ShardedCache target{2};
  const auto load = cachex::parse_command("LOAD");
  CHECK(load.ok);
  CHECK_EQ(cachex::execute(target, load.command, &persistence), ":2\n");
  CHECK_EQ(target.get("b").value_or(""), "2");

  std::remove(path.c_str());
}

CACHEX_TEST(save_and_load_without_persistence_are_errors_not_crashes) {
  cachex::ShardedCache cache{1};
  const auto save = cachex::parse_command("SAVE");
  const auto load = cachex::parse_command("LOAD");
  CHECK_EQ(cachex::execute(cache, save.command, nullptr),
           "-ERR persistence is not enabled on this server\n");
  CHECK_EQ(cachex::execute(cache, load.command, nullptr),
           "-ERR persistence is not enabled on this server\n");
}

CACHEX_TEST(save_and_load_take_no_arguments) {
  // A path from the network would let any client read or overwrite any file.
  CHECK(!cachex::parse_command("SAVE /etc/passwd").ok);
  CHECK(!cachex::parse_command("LOAD ../../x").ok);
}

// --- port parsing ----------------------------------------------------------

CACHEX_TEST(parse_port_accepts_the_full_valid_range) {
  std::uint16_t port = 1;
  CHECK(cachex::parse_port("0", port));
  CHECK_EQ(port, 0);
  CHECK(cachex::parse_port("6379", port));
  CHECK_EQ(port, 6379);
  CHECK(cachex::parse_port("65535", port));
  CHECK_EQ(port, 65535);
}

CACHEX_TEST(parse_port_rejects_what_stoi_would_silently_mangle) {
  std::uint16_t port = 1234;
  CHECK(!cachex::parse_port("", port));
  CHECK(!cachex::parse_port("65536", port));   // stoi + cast: wraps to 0
  CHECK(!cachex::parse_port("70000", port));   // stoi + cast: wraps to 4464
  CHECK(!cachex::parse_port("-1", port));      // stoi + cast: becomes 65535
  CHECK(!cachex::parse_port("123abc", port));  // stoi: accepts 123
  CHECK(!cachex::parse_port(" 80", port));
  CHECK(!cachex::parse_port("+80", port));
  CHECK_EQ(port, 1234);  // untouched on failure
}

