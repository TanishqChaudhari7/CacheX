#include "cachex/connection.hpp"

#include <sys/socket.h>

#include <cerrno>
#include <utility>

#include "cachex/command_handler.hpp"
#include "cachex/protocol.hpp"

namespace cachex {
namespace {

/// Size of one recv(). Not a limit on anything -- just how much we are willing
/// to copy per syscall. Larger means fewer syscalls; 16 KiB comfortably holds a
/// burst of pipelined commands.
constexpr std::size_t kReadChunkBytes = 16 * 1024;

}  // namespace

Connection::Connection(Socket socket, ShardedCache& cache)
    : socket_(std::move(socket)), cache_(cache) {}

bool Connection::fill_buffer() {
  char chunk[kReadChunkBytes];
  const ssize_t received = ::recv(socket_.get(), chunk, sizeof(chunk), 0);

  if (received == 0) {
    // Zero is not an error and not "no data": it is end-of-stream. The peer
    // called close() or shutdown(), and no more bytes will ever arrive.
    return false;
  }
  if (received < 0) {
    // A signal interrupted the call before any data arrived. Nothing is wrong.
    if (errno == EINTR) {
      return true;
    }
    return false;
  }

  buffer_.append(chunk, static_cast<std::size_t>(received));
  return true;
}

bool Connection::handle_line(const std::string& line) {
  const ParseResult parsed = parse_command(line);

  if (!parsed.ok) {
    // A malformed command is a normal event, not a reason to hang up: the
    // client is told what was wrong and the connection continues. Only
    // framing violations (below) end the connection.
    return send_all(socket_.get(), reply_error(parsed.error));
  }

  ++commands_handled_;
  const std::string reply = execute(cache_, parsed.command);
  if (!send_all(socket_.get(), reply)) {
    return false;  // the peer went away mid-write
  }

  // QUIT is acknowledged first, then the connection ends -- the client should
  // see "+BYE" before the socket closes under it.
  return parsed.command.type != CommandType::Quit;
}

void Connection::serve() {
  while (true) {
    // Drain every complete line already buffered *before* asking the kernel for
    // more. One recv() can deliver several commands, and a client that pipelines
    // would hang forever if we served only one request per read.
    while (const std::optional<std::string> line = buffer_.next_line()) {
      if (!handle_line(*line)) {
        return;
      }
    }

    // No complete line available. If what is already buffered exceeds the limit,
    // the client is sending a line we are never going to accept, so there is no
    // point reading more of it.
    if (buffer_.buffered() > kMaxLineBytes) {
      // The connection is closed rather than resynchronised. Recovering would
      // mean discarding bytes until the next '\n' -- doable, but this is a
      // protocol violation, and Redis closes the connection here too. Sending
      // the error first means the client learns why.
      send_all(socket_.get(), reply_error("line too long (maximum " +
                                          std::to_string(kMaxLineBytes) +
                                          " bytes); closing connection"));
      return;
    }

    if (!fill_buffer()) {
      return;  // client disconnected, or the socket failed
    }
  }
}

}  // namespace cachex
