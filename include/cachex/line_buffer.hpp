#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace cachex {

/// Turns a TCP byte stream back into lines.
///
/// This class exists because **TCP has no message boundaries**. It is a stream
/// of bytes, not a sequence of messages: a single recv() can hand back half a
/// command, three commands, or three and a half. Anything that reads commands
/// off a socket therefore needs to buffer what it got, hand out only the
/// complete lines, and keep the remainder for next time.
///
/// Keeping that logic here — away from any socket — means the awkward cases
/// (a line split across two reads, several lines in one read, a lone "\r")
/// can be unit-tested by calling append() with whatever fragments we like.
class LineBuffer {
 public:
  /// Adds freshly received bytes.
  void append(const char* data, std::size_t size);
  void append(std::string_view data);

  /// Extracts the next complete line, or nullopt if no '\n' has arrived yet.
  ///
  /// The terminator is removed, and a trailing '\r' is stripped as well so that
  /// CRLF clients (telnet, and most hand-written ones) work without a special
  /// case. The returned line may legitimately be empty — "\n\n" is two empty
  /// lines, not one.
  std::optional<std::string> next_line();

  /// Bytes held but not yet forming a complete line. The caller uses this to
  /// enforce a maximum line length: if this exceeds the limit and no line has
  /// appeared, the peer is sending something we will never accept.
  std::size_t buffered() const noexcept { return buffer_.size(); }

  bool empty() const noexcept { return buffer_.empty(); }
  void clear() noexcept { buffer_.clear(); }

 private:
  std::string buffer_;
};

}  // namespace cachex
