#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include "cachex/persistence.hpp"
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
  std::cerr << "usage: " << program
            << " [port] [capacity] [shards] [snapshot] [save-secs]\n"
            << "  port       TCP port to listen on (default 6379)\n"
            << "  capacity   maximum entries total, 0 for unbounded (default 0)\n"
            << "  shards     independently locked shards (default 8)\n"
            << "  snapshot   snapshot file path; enables SAVE/LOAD (default off)\n"
            << "  save-secs  auto-save interval in seconds, 0 to disable\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::uint16_t port = 6379;
  std::size_t capacity = 0;
  std::size_t shards = 8;
  std::string snapshot_path;
  int save_interval = 0;

  if (argc > 6) {
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
    if (argc >= 5) {
      snapshot_path = argv[4];
    }
    if (argc >= 6) {
      save_interval = std::stoi(argv[5]);
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

  // Persistence is opt-in: without a path, SAVE and LOAD report that it is not
  // enabled rather than silently writing somewhere unexpected.
  std::unique_ptr<cachex::PersistenceManager> persistence;
  if (!snapshot_path.empty()) {
    persistence = std::make_unique<cachex::PersistenceManager>(snapshot_path);

    const cachex::PersistenceManager::LoadResult restored =
        persistence->load(*cache);
    if (!restored.ok) {
      // A corrupt snapshot is reported and the server starts empty rather than
      // refusing to run -- a cache that cannot start is worse than a cold one.
      std::cerr << "cachex-server: ignoring snapshot: " << restored.error << "\n";
    } else if (restored.loaded > 0 || restored.expired > 0) {
      std::cout << "restored " << restored.loaded << " entries from "
                << snapshot_path << " (" << restored.expired
                << " already expired)\n";
    }
  }

  cachex::Server::Options options;
  options.port = port;
  cachex::Server server(*cache, options, persistence.get());

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
            << "snapshot: "
            << (snapshot_path.empty() ? std::string("disabled")
                                      : snapshot_path +
                                            (save_interval > 0
                                                 ? " (auto-save every " +
                                                       std::to_string(save_interval) +
                                                       "s)"
                                                 : " (SAVE/LOAD only)"))
            << "\n"
            << "thread-per-connection, max " << options.max_connections
            << " concurrent clients\n"
            << "press Ctrl-C to stop\n"
            << std::flush;

  // Started after the server is listening so a slow first save cannot delay
  // accepting clients; destroyed before the cache, so it never saves a corpse.
  std::unique_ptr<cachex::PeriodicSaver> saver;
  if (persistence && save_interval > 0) {
    saver = std::make_unique<cachex::PeriodicSaver>(
        *persistence, *cache, std::chrono::seconds(save_interval));
  }

  server.run();

  if (saver) {
    saver->stop();
  }

  std::cout << "\n[cachex] shutting down after " << server.connections_served()
            << " connection(s); " << cache->size() << " entries in cache\n";

  if (persistence) {
    const cachex::PersistenceManager::SaveResult saved = persistence->save(*cache);
    if (saved.ok) {
      std::cout << "[cachex] saved " << saved.entries << " entries to "
                << snapshot_path << " (" << saved.bytes << " bytes)\n";
    } else {
      std::cerr << "[cachex] final save failed: " << saved.error << "\n";
      return 1;
    }
  }
  return 0;
}
