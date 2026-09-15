#include <sys/socket.h>

#include <cstdlib>
#include <iostream>
#include <string>

#include "cachex/line_buffer.hpp"
#include "cachex/socket.hpp"
#include "cachex/version.hpp"

namespace {

/// Reads one reply line, buffering across recv() calls.
///
/// The client has exactly the same framing problem as the server: a reply may
/// arrive split across two packets, or two replies may arrive together. It uses
/// the same LineBuffer for the same reason.
bool read_reply(int fd, cachex::LineBuffer& buffer, std::string& line) {
  while (true) {
    if (std::optional<std::string> complete = buffer.next_line()) {
      line = std::move(*complete);
      return true;
    }
    char chunk[4096];
    const ssize_t received = ::recv(fd, chunk, sizeof(chunk), 0);
    if (received <= 0) {
      return false;  // server closed, or the socket failed
    }
    buffer.append(chunk, static_cast<std::size_t>(received));
  }
}

void print_banner(const std::string& host, std::uint16_t port) {
  std::cout << "CacheX client " << cachex::version_string() << " connected to "
            << host << ":" << port << "\n"
            << "commands: SET key value [ttl_seconds] | GET key | DELETE key\n"
            << "          EXISTS key | TTL key | SAVE | LOAD | PING | QUIT\n"
            << "Ctrl-D or QUIT to exit\n\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  std::uint16_t port = 6379;

  if (argc > 3) {
    std::cerr << "usage: " << argv[0] << " [host] [port]\n";
    return 2;
  }
  if (argc >= 2) {
    host = argv[1];
  }
  if (argc >= 3 && (!cachex::parse_port(argv[2], port) || port == 0)) {
    std::cerr << "cachex-client: invalid port '" << argv[2] << "'\n";
    return 2;
  }

  std::string error;
  cachex::Socket socket = cachex::connect_to(host, port, error);
  if (!socket.valid()) {
    std::cerr << "cachex-client: cannot connect to " << host << ":" << port
              << ": " << error << "\n";
    return 1;
  }

  print_banner(host, port);

  cachex::LineBuffer replies;
  std::string request;
  while (true) {
    std::cout << "cachex> " << std::flush;
    if (!std::getline(std::cin, request)) {
      std::cout << "\n";  // Ctrl-D
      break;
    }
    if (request.empty()) {
      continue;  // don't bother the server with a blank line
    }

    // The protocol is line-based, so every request needs its terminator. This
    // is the framing the server relies on to know where the command ends.
    if (!cachex::send_all(socket.get(), request + "\n")) {
      std::cerr << "cachex-client: connection lost while sending\n";
      return 1;
    }

    std::string reply;
    if (!read_reply(socket.get(), replies, reply)) {
      std::cerr << "cachex-client: server closed the connection\n";
      return 1;
    }
    std::cout << reply << "\n";

    if (reply == "+BYE") {
      break;
    }
  }
  return 0;
}
