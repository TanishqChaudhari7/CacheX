#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cachex/sharded_cache.hpp"

namespace cachex {

/// Reads and writes cache snapshots.
///
///      ShardedCache  ──export_entries()──►  PersistenceManager  ──►  file
///           ▲                                      │
///           └──────────────  set()  ───────────────┘
///
/// It talks to the cache only through its public API: export_entries() to read,
/// and ordinary set() calls to restore. It never touches a map, a list or an
/// iterator, and it knows nothing about sockets -- the SAVE/LOAD commands are
/// wired up in the network layer, not here.
class PersistenceManager {
 public:
  struct SaveResult {
    bool ok = false;
    std::size_t entries = 0;
    std::size_t bytes = 0;
    std::string error;
  };

  struct LoadResult {
    bool ok = false;
    std::size_t loaded = 0;
    /// Entries in the file whose TTL had run out before the load.
    std::size_t expired = 0;
    std::string error;
  };

  explicit PersistenceManager(std::string path) : path_(std::move(path)) {}

  const std::string& path() const noexcept { return path_; }

  /// Writes every live entry to the snapshot file.
  ///
  /// The write goes to `path + ".tmp"` and is then renamed into place. rename()
  /// is atomic on POSIX, so an interrupted save leaves either the previous
  /// snapshot or the new one -- never a half-written file that would fail to
  /// load. Writing in place would risk exactly that.
  SaveResult save(const ShardedCache& cache) const;

  /// Replaces nothing; adds the snapshot's entries to `cache` via set().
  ///
  /// The file is parsed and validated **completely before anything is applied**,
  /// so a malformed snapshot leaves the cache untouched rather than half
  /// populated. Entries whose remaining TTL has run out since the save are
  /// counted and skipped.
  ///
  /// A missing file is not an error: it is reported as ok with zero entries,
  /// because "no snapshot yet" is the normal state on a first start.
  LoadResult load(ShardedCache& cache) const;

  /// True if the snapshot file exists and is readable.
  bool exists() const;

 private:
  std::string path_;
};

/// Saves the cache on a fixed interval, on its own thread.
///
/// The thread waits on a condition variable rather than sleeping, so stopping is
/// immediate instead of taking up to a whole interval. It is joined in the
/// destructor, never detached -- a detached saver could outlive the cache it
/// holds a reference to.
///
/// Nothing here needs extra locking: it calls the same public save() a client's
/// SAVE command would, and the cache does its own synchronisation.
class PeriodicSaver {
 public:
  PeriodicSaver(PersistenceManager& manager, ShardedCache& cache,
                std::chrono::seconds interval);
  ~PeriodicSaver();

  PeriodicSaver(const PeriodicSaver&) = delete;
  PeriodicSaver& operator=(const PeriodicSaver&) = delete;

  void stop();

  std::size_t saves_completed() const noexcept { return saves_.load(); }
  std::size_t save_failures() const noexcept { return failures_.load(); }

 private:
  void run();

  PersistenceManager& manager_;
  ShardedCache& cache_;
  std::chrono::seconds interval_;

  std::mutex mutex_;
  std::condition_variable wake_;
  bool stopping_ = false;

  std::atomic<std::size_t> saves_{0};
  std::atomic<std::size_t> failures_{0};
  std::thread thread_;
};

}  // namespace cachex
