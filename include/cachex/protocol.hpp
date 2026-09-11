#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace cachex {

/// Longest request line accepted, terminator included.
///
/// Without a limit, a client that never sends '\n' would make the server buffer
/// until it ran out of memory -- one line of code between a working server and a
/// trivial denial of service. 64 KiB is far more than any legitimate command in
/// this protocol needs.
inline constexpr std::size_t kMaxLineBytes = 64 * 1024;

/// Ten years. Guards against a TTL large enough to overflow the deadline
/// arithmetic, and nothing legitimate needs more.
inline constexpr long long kMaxTtlSeconds = 315360000;

enum class CommandType { Set, Get, Delete, Exists, Ttl, Ping, Quit };

struct Command {
  CommandType type = CommandType::Ping;
  std::string key;
  std::string value;
  std::optional<std::chrono::seconds> ttl;  ///< SET only
};

/// Deliberately not exceptions: a malformed command is an ordinary, expected
/// event on a public socket -- clients send garbage all the time -- not an
/// exceptional condition. It also keeps the parser usable from code that has
/// exceptions disabled.
struct ParseResult {
  bool ok = false;
  Command command{};
  std::string error;  ///< reason when ok is false; already client-readable
};

/// Parses one request line. The line must already have its terminator removed
/// (that is LineBuffer's job) -- this function never sees a '\n' and knows
/// nothing about sockets, which is what makes it directly testable.
ParseResult parse_command(std::string_view line);

const char* command_name(CommandType type);

// --- Response formatting ---------------------------------------------------
//
// Every reply is one line and begins with a one-byte type tag, so a client can
// tell replies apart by looking at a single character. Each of these returns a
// complete reply including the trailing '\n'.

std::string reply_ok();                              ///< "+OK"
std::string reply_pong();                            ///< "+PONG"
std::string reply_bye();                             ///< "+BYE"
std::string reply_no_expiry();                       ///< "+NOEXPIRE"
std::string reply_value(std::string_view value);     ///< "=<value>"
std::string reply_nil();                             ///< "_"
std::string reply_integer(long long value);          ///< ":<n>"
std::string reply_error(std::string_view message);   ///< "-ERR <message>"

}  // namespace cachex
