#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace cachex {

/// RAII owner of a socket file descriptor.
///
/// A file descriptor is a resource exactly like heap memory, and it obeys the
/// same ownership rules: exactly one owner, released in a destructor. Copying is
/// deleted because two owners would both close the same descriptor -- and the
/// second close would be a *use-after-free of a number*: the OS may already have
/// handed that integer out for a different file, so the second close would shut
/// down something unrelated. Moving transfers ownership and leaves the source
/// invalid.
class Socket {
 public:
  static constexpr int kInvalid = -1;

  Socket() = default;
  explicit Socket(int fd) noexcept : fd_(fd) {}
  ~Socket() { close(); }

  Socket(Socket&& other) noexcept : fd_(other.fd_) { other.fd_ = kInvalid; }
  Socket& operator=(Socket&& other) noexcept;

  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  int get() const noexcept { return fd_; }
  bool valid() const noexcept { return fd_ != kInvalid; }
  explicit operator bool() const noexcept { return valid(); }

  void close() noexcept;

 private:
  int fd_ = kInvalid;
};

/// Writes the whole buffer, looping until it is gone.
///
/// send() is allowed to accept fewer bytes than offered -- the kernel's send
/// buffer may simply be full -- so a single send() is never enough. Ignoring the
/// return value is one of the classic socket bugs: the message goes out
/// truncated and the peer sees a corrupted stream.
bool send_all(int fd, std::string_view data);

/// Small conveniences shared by the server, the client and the benchmark.
void set_tcp_nodelay(int fd);
void set_no_sigpipe(int fd);

/// Resolves host/port and connects. Returns an invalid Socket on failure, with
/// `error` describing why.
Socket connect_to(const std::string& host, std::uint16_t port, std::string& error);

/// Parses a TCP port strictly: digits only, 0-65535, nothing trailing.
///
/// The obvious static_cast<uint16_t>(std::stoi(text)) is wrong three ways: it
/// accepts "123abc", it silently wraps 70000 to 4464, and -1 becomes 65535 -- so
/// a typo starts the server on a port nobody asked for.
bool parse_port(std::string_view text, std::uint16_t& port);

}  // namespace cachex
