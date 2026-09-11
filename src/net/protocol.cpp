#include "cachex/protocol.hpp"

#include <charconv>
#include <vector>

namespace cachex {
namespace {

bool is_separator(char c) { return c == ' ' || c == '\t'; }

/// Splits on runs of spaces/tabs, skipping empties, so "GET   foo" parses the
/// same as "GET foo". Views into `line`, so nothing is copied while parsing.
std::vector<std::string_view> tokenize(std::string_view line) {
  std::vector<std::string_view> tokens;
  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && is_separator(line[i])) {
      ++i;
    }
    if (i >= line.size()) {
      break;
    }
    const std::size_t start = i;
    while (i < line.size() && !is_separator(line[i])) {
      ++i;
    }
    tokens.push_back(line.substr(start, i - start));
  }
  return tokens;
}

std::string to_upper(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
  }
  return out;
}

ParseResult fail(std::string message) {
  ParseResult result;
  result.ok = false;
  result.error = std::move(message);
  return result;
}

ParseResult succeed(Command command) {
  ParseResult result;
  result.ok = true;
  result.command = std::move(command);
  return result;
}

std::string quoted(std::string_view text) {
  // Truncated so a 60 KB junk token cannot be echoed back in full.
  constexpr std::size_t kMaxEcho = 32;
  std::string out = "'";
  out.append(text.substr(0, kMaxEcho));
  if (text.size() > kMaxEcho) {
    out += "...";
  }
  out += "'";
  return out;
}

/// from_chars rather than stoll/atoi: no exceptions, no locale, no silent
/// partial parse. "12abc" is rejected here, where atoi would happily return 12.
bool parse_integer(std::string_view text, long long& out) {
  if (text.empty()) {
    return false;
  }
  const char* const begin = text.data();
  const char* const end = begin + text.size();
  const std::from_chars_result result = std::from_chars(begin, end, out);
  return result.ec == std::errc() && result.ptr == end;
}

ParseResult parse_key_only(CommandType type, const char* name,
                           const std::vector<std::string_view>& tokens) {
  if (tokens.size() != 2) {
    return fail(std::string("wrong number of arguments for '") + name +
                "' (expected '" + name + " key')");
  }
  Command command;
  command.type = type;
  command.key = std::string(tokens[1]);
  return succeed(std::move(command));
}

ParseResult parse_no_args(CommandType type, const char* name,
                          const std::vector<std::string_view>& tokens) {
  if (tokens.size() != 1) {
    return fail(std::string("wrong number of arguments for '") + name +
                "' (expected '" + name + "')");
  }
  Command command;
  command.type = type;
  return succeed(std::move(command));
}

}  // namespace

ParseResult parse_command(std::string_view line) {
  const std::vector<std::string_view> tokens = tokenize(line);
  if (tokens.empty()) {
    return fail("empty command");
  }

  // Case-insensitive verbs, like Redis: typing `get foo` by hand should work.
  const std::string verb = to_upper(tokens[0]);

  if (verb == "SET") {
    if (tokens.size() < 3 || tokens.size() > 4) {
      return fail(
          "wrong number of arguments for 'SET' (expected 'SET key value "
          "[ttl_seconds]')");
    }
    Command command;
    command.type = CommandType::Set;
    command.key = std::string(tokens[1]);
    command.value = std::string(tokens[2]);

    if (tokens.size() == 4) {
      long long seconds = 0;
      if (!parse_integer(tokens[3], seconds)) {
        return fail("invalid TTL " + quoted(tokens[3]) +
                    " (expected a whole number of seconds)");
      }
      // A negative TTL is almost certainly a client bug, so it is rejected
      // rather than quietly treated as a delete. Zero is accepted and *does*
      // delete, matching the cache's documented semantics.
      if (seconds < 0) {
        return fail("TTL must not be negative");
      }
      if (seconds > kMaxTtlSeconds) {
        return fail("TTL too large (maximum " + std::to_string(kMaxTtlSeconds) +
                    " seconds)");
      }
      command.ttl = std::chrono::seconds(seconds);
    }
    return succeed(std::move(command));
  }

  if (verb == "GET") {
    return parse_key_only(CommandType::Get, "GET", tokens);
  }
  if (verb == "DELETE" || verb == "DEL") {
    return parse_key_only(CommandType::Delete, "DELETE", tokens);
  }
  if (verb == "EXISTS") {
    return parse_key_only(CommandType::Exists, "EXISTS", tokens);
  }
  if (verb == "TTL") {
    return parse_key_only(CommandType::Ttl, "TTL", tokens);
  }
  if (verb == "PING") {
    return parse_no_args(CommandType::Ping, "PING", tokens);
  }
  // SAVE and LOAD take no arguments *on purpose*: the snapshot path is server
  // configuration, never something a client supplies. Accepting a path from the
  // network would let any client read or overwrite an arbitrary file.
  if (verb == "SAVE") {
    return parse_no_args(CommandType::Save, "SAVE", tokens);
  }
  if (verb == "LOAD") {
    return parse_no_args(CommandType::Load, "LOAD", tokens);
  }
  if (verb == "QUIT") {
    return parse_no_args(CommandType::Quit, "QUIT", tokens);
  }

  return fail("unknown command " + quoted(tokens[0]));
}

const char* command_name(CommandType type) {
  switch (type) {
    case CommandType::Set:
      return "SET";
    case CommandType::Get:
      return "GET";
    case CommandType::Delete:
      return "DELETE";
    case CommandType::Exists:
      return "EXISTS";
    case CommandType::Ttl:
      return "TTL";
    case CommandType::Ping:
      return "PING";
    case CommandType::Save:
      return "SAVE";
    case CommandType::Load:
      return "LOAD";
    case CommandType::Quit:
      return "QUIT";
  }
  return "UNKNOWN";
}

std::string reply_ok() { return "+OK\n"; }
std::string reply_pong() { return "+PONG\n"; }
std::string reply_bye() { return "+BYE\n"; }
std::string reply_no_expiry() { return "+NOEXPIRE\n"; }

std::string reply_value(std::string_view value) {
  std::string out = "=";
  out.append(value);
  out += "\n";
  return out;
}

std::string reply_nil() { return "_\n"; }

std::string reply_integer(long long value) {
  return ":" + std::to_string(value) + "\n";
}

std::string reply_error(std::string_view message) {
  std::string out = "-ERR ";
  out.append(message);
  out += "\n";
  return out;
}

}  // namespace cachex
