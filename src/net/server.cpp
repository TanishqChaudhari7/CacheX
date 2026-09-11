#include "cachex/server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <utility>

#include "cachex/protocol.hpp"

#include "cachex/connection.hpp"

namespace cachex {
namespace {

/// How long accept() waits before the loop re-checks the stop flag. Short
/// enough that shutdown feels immediate, long enough that idling costs nothing.
constexpr int kAcceptPollMs = 100;

}  // namespace

Server::Server(SyncCache& cache, Options options)
    : cache_(cache), options_(std::move(options)) {}

bool Server::start(std::string& error) {
  // 1. socket() -- create an endpoint. It is not yet attached to any address.
  listener_ = Socket(::socket(AF_INET, SOCK_STREAM, 0));
  if (!listener_.valid()) {
    error = std::string("socket() failed: ") + std::strerror(errno);
    return false;
  }

  // SO_REUSEADDR lets us re-bind a port still held in TIME_WAIT by a previous
  // process. After a server exits, its closed connections linger in TIME_WAIT
  // for up to a couple of minutes so that late duplicate packets cannot be
  // delivered to a new connection; without this option, restarting the server
  // fails with "Address already in use" for that whole window.
  const int on = 1;
  ::setsockopt(listener_.get(), SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(options_.port);  // network byte order, always
  if (::inet_pton(AF_INET, options_.host.c_str(), &address.sin_addr) != 1) {
    error = "invalid bind address '" + options_.host + "'";
    return false;
  }

  // 2. bind() -- claim the address and port.
  if (::bind(listener_.get(), reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) != 0) {
    error = std::string("bind() failed: ") + std::strerror(errno);
    if (errno == EADDRINUSE) {
      // 6379 is Redis's port, and this project's default. If a real Redis is
      // running, a client that connects anyway will get RESP replies and very
      // confusing results, so say so rather than just reporting the errno.
      error +=
          "\n  Another process already holds " + options_.host + ":" +
          std::to_string(options_.port) +
          ". Note that 6379 is also Redis's default port -- check with\n"
          "  `lsof -nP -iTCP:" + std::to_string(options_.port) +
          " -sTCP:LISTEN`, or start CacheX on a different port.";
    }
    return false;
  }

  // 3. listen() -- mark the socket passive. This does NOT block and does not
  // accept anything; it tells the kernel to start completing TCP handshakes on
  // our behalf and to queue the finished connections, up to `backlog` of them.
  if (::listen(listener_.get(), options_.backlog) != 0) {
    error = std::string("listen() failed: ") + std::strerror(errno);
    return false;
  }

  // With port 0 the kernel picked a free port; ask which one it chose.
  sockaddr_in actual{};
  socklen_t actual_len = sizeof(actual);
  if (::getsockname(listener_.get(), reinterpret_cast<sockaddr*>(&actual),
                    &actual_len) == 0) {
    bound_port_ = ntohs(actual.sin_port);
  } else {
    bound_port_ = options_.port;
  }
  return true;
}

void Server::run() {
  while (!stop_requested_.load()) {
    // accept() blocks indefinitely, which would make the server unstoppable.
    // poll() with a timeout turns the wait into a bounded one so the loop can
    // notice stop() between attempts -- and can reap finished workers.
    pollfd waiting{};
    waiting.fd = listener_.get();
    waiting.events = POLLIN;

    const int ready = ::poll(&waiting, 1, kAcceptPollMs);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }

    reap_finished_workers();

    if (ready == 0) {
      continue;  // nothing waiting; re-check the stop flag
    }

    // 4. accept() -- take one completed connection off the queue. It returns a
    // *new* socket for that client; the listening socket stays open and keeps
    // accepting. Two different sockets, two different jobs.
    Socket client(::accept(listener_.get(), nullptr, nullptr));
    if (!client.valid()) {
      continue;  // the client may have vanished between poll() and accept()
    }

    set_tcp_nodelay(client.get());
    set_no_sigpipe(client.get());

    if (active_connections_.load() >= options_.max_connections) {
      // Thread-per-connection means one more client is one more thread. Refuse
      // politely rather than letting the thread count run away.
      send_all(client.get(),
               reply_error("server at connection limit (" +
                           std::to_string(options_.max_connections) + ")"));
      continue;  // client's destructor closes the socket
    }

    spawn_worker(std::move(client));
  }

  // The accept loop is done. Every worker holds a reference to cache_ and to
  // this Server, so none of them may outlive run() returning.
  join_all_workers();
}

void Server::spawn_worker(Socket client) {
  auto finished = std::make_shared<std::atomic<bool>>(false);

  const std::size_t id = connections_served_.fetch_add(1) + 1;
  active_connections_.fetch_add(1);

  if (options_.verbose) {
    std::cout << "[cachex] client connected (#" << id << ", "
              << active_connections_.load() << " active)\n"
              << std::flush;
  }

  std::thread worker([this, socket = std::move(client), finished, id]() mutable {
    Connection connection(std::move(socket), cache_);
    connection.serve();

    if (options_.verbose) {
      std::cout << "[cachex] client #" << id << " disconnected after "
                << connection.commands_handled() << " command(s)\n"
                << std::flush;
    }
    active_connections_.fetch_sub(1);

    // Set last: once this is true the accept loop may join and destroy this
    // Worker, so nothing after it may touch the thread's own state.
    finished->store(true);
  });

  const std::lock_guard<std::mutex> lock(workers_mutex_);
  workers_.push_back(Worker{std::move(worker), std::move(finished)});
}

void Server::reap_finished_workers() {
  const std::lock_guard<std::mutex> lock(workers_mutex_);
  for (std::size_t i = 0; i < workers_.size();) {
    if (workers_[i].finished->load()) {
      // join() on an already-finished thread returns immediately; it is what
      // releases the thread's resources. Detaching instead would mean a worker
      // could outlive the Server and the cache it references.
      workers_[i].thread.join();
      workers_.erase(workers_.begin() + static_cast<std::ptrdiff_t>(i));
    } else {
      ++i;
    }
  }
}

void Server::join_all_workers() {
  std::vector<Worker> remaining;
  {
    const std::lock_guard<std::mutex> lock(workers_mutex_);
    remaining.swap(workers_);
  }
  // Joined outside the lock: a worker that finishes here would otherwise try to
  // take workers_mutex_ through reap, and blocking a join on a lock the joining
  // thread already holds is how deadlocks get written.
  for (Worker& worker : remaining) {
    if (worker.thread.joinable()) {
      worker.thread.join();
    }
  }
}

}  // namespace cachex
