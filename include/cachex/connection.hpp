#pragma once

#include "cachex/sync_cache.hpp"
#include "cachex/line_buffer.hpp"
#include "cachex/socket.hpp"

namespace cachex {

/// Serves one client for the whole life of its connection.
///
/// The read loop is where the interesting part of TCP shows up: recv() returns
/// whatever bytes have arrived, which may be a fraction of a command or several
/// commands at once, so the connection buffers and re-frames rather than
/// assuming one read equals one request.
class Connection {
 public:
  Connection(Socket socket, SyncCache& cache);

  /// Reads commands and writes replies until the client disconnects, sends
  /// QUIT, or breaks the protocol. Blocking; returns when the connection is
  /// finished. The socket is closed by the destructor.
  void serve();

  /// Commands executed on this connection. Used by tests and the server log.
  std::size_t commands_handled() const noexcept { return commands_handled_; }

 private:
  /// One recv() into the buffer. False means the peer closed or the socket
  /// failed -- either way, this connection is over.
  bool fill_buffer();

  /// Handles one request line. False means stop serving this connection.
  bool handle_line(const std::string& line);

  Socket socket_;
  SyncCache& cache_;
  LineBuffer buffer_;
  std::size_t commands_handled_ = 0;
};

}  // namespace cachex
