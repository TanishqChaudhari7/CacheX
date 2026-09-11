#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include "cachex/sharded_cache.hpp"
#include "cachex/server.hpp"
#include "cachex/version.hpp"

namespace {

cachex::Server* g_server = nullptr;

// Async-signal-safe work only: set a flag via the server's atomic and return.
// Anything else here (allocating, printing, locking) is undefined behaviour in
// a signal handler.
void handle_signal(int) {
  if (g_server != nullptr) {
    g_server->stop();
  }
}

void usage(const char* program) {
  std::cerr << "usage: " << program << " [port] [capacity] [shards]\n"
            << "  port      TCP port to listen on (default 6379)\n"
            << "  capacity  maximum entries total, 0 for unbounded (default 0)\n"
            << "  shards    independently locked shards (default 8)\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::uint16_t port = 6379;
  std::size_t capacity = 0;
  std::size_t shards = 8;

  if (argc > 4) {
    usage(argv[0]);
    return 2;
  }
  try {
    if (argc >= 2) {
      port = static_cast<std::uint16_t>(std::stoi(argv[1]));
    }
    if (argc >= 3) {
      capacity = static_cast<std::size_t>(std::stoull(argv[2]));
    }
    if (argc >= 4) {
      shards = static_cast<std::size_t>(std::stoull(argv[3]));
    }
  } catch (const std::exception&) {
    usage(argv[0]);
    return 2;
  }

  // unique_ptr because ShardedCache owns mutexes and so is neither copyable nor
  // movable -- the two constructors cannot be selected with a ternary.
  const auto cache =
      capacity > 0 ? std::make_unique<cachex::ShardedCache>(shards, capacity)
                   : std::make_unique<cachex::ShardedCache>(shards);

  cachex::Server::Options options;
  options.port = port;
  cachex::Server server(*cache, options);

  std::string error;
  if (!server.start(error)) {
    std::cerr << "cachex-server: " << error << "\n";
    return 1;
  }

  g_server = &server;
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  std::cout << "CacheX " << cachex::version_string() << " listening on "
            << options.host << ":" << server.bound_port() << "\n"
            << "capacity: "
            << (capacity > 0 ? std::to_string(capacity) + " entries"
                             : std::string("unbounded"))
            << "\n"
            << "shards: " << cache->shard_count() << "\n"
            << "thread-per-connection, max " << options.max_connections
            << " concurrent clients\n"
            << "press Ctrl-C to stop\n"
            << std::flush;

  server.run();

  std::cout << "\n[cachex] shutting down after " << server.connections_served()
            << " connection(s); " << cache->size() << " entries in cache\n";
  return 0;
}
