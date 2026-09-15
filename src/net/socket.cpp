#include "cachex/socket.hpp"

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstring>

namespace cachex {
namespace {

// Writing to a socket whose peer has gone away raises SIGPIPE, whose default
// action is to kill the process. A server that dies because a client hung up is
// obviously unacceptable. Linux suppresses it per-call with MSG_NOSIGNAL;
// macOS/BSD has no such flag and uses the SO_NOSIGPIPE socket option instead.
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

}  // namespace

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    close();
    fd_ = other.fd_;
    other.fd_ = kInvalid;
  }
  return *this;
}

void Socket::close() noexcept {
  if (fd_ != kInvalid) {
    ::close(fd_);
    fd_ = kInvalid;
  }
}

bool send_all(int fd, std::string_view data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t written =
        ::send(fd, data.data() + sent, data.size() - sent, kSendFlags);
    if (written < 0) {
      // EINTR means a signal arrived mid-call, not that anything failed --
      // retry. Treating it as an error is a classic source of rare, unexplained
      // dropped connections.
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    sent += static_cast<std::size_t>(written);
  }
  return true;
}

void set_tcp_nodelay(int fd) {
  // Disables Nagle's algorithm. Nagle holds small outgoing packets back, waiting
  // to coalesce them with the next write. For a bulk transfer that is a win; for
  // a request/response protocol it is a disaster -- it interacts with delayed
  // ACKs to add up to ~40 ms per round trip, which would dominate every number
  // the benchmark produces.
  const int on = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}

void set_no_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
  const int on = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
  (void)fd;  // Linux uses MSG_NOSIGNAL on each send instead.
#endif
}

Socket connect_to(const std::string& host, std::uint16_t port,
                  std::string& error) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;  // accept IPv4 or IPv6, whichever resolves
  hints.ai_socktype = SOCK_STREAM;

  const std::string port_text = std::to_string(port);
  addrinfo* results = nullptr;
  const int rc = ::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &results);
  if (rc != 0) {
    error = "cannot resolve '" + host + "': " + ::gai_strerror(rc);
    return Socket{};
  }

  // A hostname can resolve to several addresses; try each until one connects.
  for (addrinfo* candidate = results; candidate != nullptr;
       candidate = candidate->ai_next) {
    Socket socket(::socket(candidate->ai_family, candidate->ai_socktype,
                           candidate->ai_protocol));
    if (!socket.valid()) {
      continue;
    }
    if (::connect(socket.get(), candidate->ai_addr, candidate->ai_addrlen) == 0) {
      ::freeaddrinfo(results);
      set_tcp_nodelay(socket.get());
      set_no_sigpipe(socket.get());
      return socket;
    }
    error = std::strerror(errno);
  }

  ::freeaddrinfo(results);
  if (error.empty()) {
    error = "no usable address";
  }
  return Socket{};
}

bool parse_port(std::string_view text, std::uint16_t& port) {
  unsigned long value = 0;
  const char* const end = text.data() + text.size();
  const std::from_chars_result parsed = std::from_chars(text.data(), end, value);
  if (text.empty() || parsed.ec != std::errc() || parsed.ptr != end || value > 65535) {
    return false;
  }
  port = static_cast<std::uint16_t>(value);
  return true;
}

}  // namespace cachex
