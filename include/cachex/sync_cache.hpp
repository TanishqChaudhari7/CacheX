#pragma once

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "cachex/cache.hpp"

namespace cachex {

/// A thread-safe Cache: one coarse-grained mutex around the whole thing.
///
/// Deliberately a wrapper rather than a mutex bolted inside Cache. Cache stays
/// single-threaded and pays nothing, so the in-process benchmarks keep measuring
/// the data structure rather than the lock, and the locking is visible in one
/// place instead of scattered through the engine.
///
/// **Every method takes an exclusive lock, including get().** That is not
/// laziness: under LRU a read *is* a write. get() splices the entry to the head
/// of the recency list and may reclaim an expired one, so two concurrent "reads"
/// would be two concurrent list mutations. A std::shared_mutex would therefore
/// let almost nothing actually share -- reads are the common case in a cache,
/// and reads are writes here. See ARCHITECTURE §8.
///
/// The type is the safety mechanism: Server and Connection take a SyncCache&,
/// so it is not possible to hand the server an unsynchronised Cache by mistake.
class SyncCache {
 public:
  using Duration = Cache::Duration;

  SyncCache() = default;
  explicit SyncCache(std::size_t capacity) : cache_(capacity) {}

  SyncCache(const SyncCache&) = delete;
  SyncCache& operator=(const SyncCache&) = delete;

  void set(std::string key, std::string value) {
    const std::lock_guard<std::mutex> lock(mutex_);
    cache_.set(std::move(key), std::move(value));
  }

  void set(std::string key, std::string value, Duration ttl) {
    const std::lock_guard<std::mutex> lock(mutex_);
    cache_.set(std::move(key), std::move(value), ttl);
  }

  /// Exclusive, not shared -- see the class comment.
  std::optional<std::string> get(const std::string& key) {
    const std::lock_guard<std::mutex> lock(mutex_);
    return cache_.get(key);
  }

  bool erase(const std::string& key) {
    const std::lock_guard<std::mutex> lock(mutex_);
    return cache_.erase(key);
  }

  /// Genuinely read-only on the underlying cache (it reports expiry without
  /// reclaiming), so this is the one operation a shared_mutex could let run in
  /// parallel. It still locks exclusively here, because this stage is the
  /// single-mutex baseline everything else gets measured against.
  bool contains(const std::string& key) {
    const std::lock_guard<std::mutex> lock(mutex_);
    return cache_.contains(key);
  }

  /// Mutates: reclaims the entry if it has expired.
  TtlInfo ttl(const std::string& key) {
    const std::lock_guard<std::mutex> lock(mutex_);
    return cache_.ttl(key);
  }

  std::size_t size() const {
    // Reading size() while another thread rehashes the map would be a data
    // race, so even the "obviously harmless" accessors take the lock.
    const std::lock_guard<std::mutex> lock(mutex_);
    return cache_.size();
  }

  bool empty() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return cache_.empty();
  }

  void clear() {
    const std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
  }

  std::optional<std::size_t> capacity() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return cache_.capacity();
  }

  std::size_t evictions() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return cache_.evictions();
  }

  std::size_t expired_removals() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return cache_.expired_removals();
  }

  std::vector<std::string> keys_by_recency() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return cache_.keys_by_recency();
  }

 private:
  // mutable so the const accessors can lock it. The mutex protects the object's
  // state, and reading state is still an operation that has to be serialised.
  mutable std::mutex mutex_;
  Cache cache_;
};

}  // namespace cachex
