#include "cachex/line_buffer.hpp"

#include <string_view>

namespace cachex {

void LineBuffer::append(const char* data, std::size_t size) {
  buffer_.append(data, size);
}

void LineBuffer::append(std::string_view data) {
  buffer_.append(data.data(), data.size());
}

std::optional<std::string> LineBuffer::next_line() {
  const std::size_t newline = buffer_.find('\n');
  if (newline == std::string::npos) {
    return std::nullopt;
  }

  std::string line = buffer_.substr(0, newline);
  // erase() from the front is O(remaining). Fine here: the buffer holds at most
  // one line's worth of leftovers plus whatever one read delivered. It would be
  // the wrong choice for a buffer holding megabytes of pipelined input.
  buffer_.erase(0, newline + 1);

  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  return line;
}

}  // namespace cachex
