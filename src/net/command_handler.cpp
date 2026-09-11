#include "cachex/command_handler.hpp"

namespace cachex {

std::string execute(SyncCache& cache, const Command& command) {
  switch (command.type) {
    case CommandType::Set:
      if (command.ttl.has_value()) {
        // seconds converts implicitly to the cache's milliseconds.
        // A TTL of 0 erases, per the cache's documented semantics.
        cache.set(command.key, command.value, *command.ttl);
      } else {
        cache.set(command.key, command.value);
      }
      return reply_ok();

    case CommandType::Get: {
      const std::optional<std::string> value = cache.get(command.key);
      return value.has_value() ? reply_value(*value) : reply_nil();
    }

    case CommandType::Delete:
      return reply_integer(cache.erase(command.key) ? 1 : 0);

    case CommandType::Exists:
      return reply_integer(cache.contains(command.key) ? 1 : 0);

    case CommandType::Ttl: {
      const TtlInfo info = cache.ttl(command.key);
      switch (info.state) {
        case TtlState::Missing:
          return reply_nil();
        case TtlState::Persistent:
          return reply_no_expiry();
        case TtlState::Expiring:
          // Round up. The cache tracks milliseconds and the protocol speaks
          // seconds, so truncating would report 0 for a key with 900 ms left --
          // and ":0" reads like "about to go", not "still alive".
          return reply_integer((info.remaining.count() + 999) / 1000);
      }
      break;
    }

    case CommandType::Ping:
      return reply_pong();

    case CommandType::Quit:
      return reply_bye();
  }

  return reply_error("internal error: unhandled command");
}

}  // namespace cachex
