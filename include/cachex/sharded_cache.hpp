#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cachex/sync_cache.hpp"

namespace cachex {

/// A cache split into independently locked shards.
///
///                        ShardedCache
///                             |
///        +--------------+-----+-----+--------------+
///        |              |           |              |
///     Shard 0        Shard 1     Shard 2   ...  Shard N-1
///   mutex+map+LRU  mutex+map+LRU   ...        mutex+map+LRU
///
/// A key is assigned to a shard by hashing it, so every operation locks exactly
/// one shard. Threads working on keys in different shards never wait for each
/// other -- which is the entire point: one global mutex makes the cache a
/// sequential section, and N shards make it N sequential sections that run in
/// parallel.
///
/// Each shard is a complete SyncCache with its own mutex, hash map, recency list
/// and capacity. Nothing is shared between shards, so there is no cross-shard
/// lock to contend on and no lock ordering to get wrong.
///
/// **The cost is that LRU becomes per-shard rather than global.** See
/// ARCHITECTURE.md §11: a hot key in a crowded shard can be evicted while a colder
/// key in a quiet shard survives.
class ShardedCache {
 public:
  using Duration = SyncCache::Duration;

  /// Unbounded, with the given number of shards.
  explicit ShardedCache(std::size_t shard_count = 8);

  /// Bounded: `total_capacity` is split as evenly as possible across the shards,
  /// and the parts sum to exactly `total_capacity`.
  ///
  /// If `total_capacity` is smaller than `shard_count`, the shard count is
  /// reduced so every shard gets at least one entry. Otherwise some shards would
  /// have capacity 0 and silently discard every key that hashed to them --
  /// a memorable bug to debug, and easy to avoid here.
  ShardedCache(std::size_t shard_count, std::size_t total_capacity);

  ShardedCache(const ShardedCache&) = delete;
  ShardedCache& operator=(const ShardedCache&) = delete;

  // --- the same API as SyncCache; callers cannot tell the difference ---------

  void set(std::string key, std::string value);
  void set(std::string key, std::string value, Duration ttl);
  std::optional<std::string> get(const std::string& key);
  /// See Cache::get_into.
  bool get_into(const std::string& key, std::string& out);
  bool erase(const std::string& key);
  bool contains(const std::string& key);
  TtlInfo ttl(const std::string& key);
  void clear();

  /// Sum over all shards.
  ///
  /// **Not a consistent snapshot.** Shards are locked one at a time, so another
  /// thread can modify a shard that has already been counted. The result is
  /// accurate on a quiescent cache and approximate under load -- which is the
  /// right trade: a globally consistent size would need a lock held across every
  /// shard at once, reintroducing exactly the bottleneck sharding removes.
  std::size_t size() const;
  bool empty() const;

  /// nullopt when unbounded; otherwise the sum of the shard capacities, which
  /// equals the total requested.
  std::optional<std::size_t> capacity() const;
  std::size_t evictions() const;
  std::size_t expired_removals() const;

  /// Every live entry across every shard.
  ///
  /// Shards are locked **one at a time**, so this is internally consistent per
  /// shard but not a single point-in-time view of the whole cache: shard 0 is
  /// read slightly before shard N-1. Locking all shards at once would give a
  /// true snapshot and would reintroduce exactly the global stall sharding
  /// exists to remove. See ARCHITECTURE.md §11 and §12.
  std::vector<EntrySnapshot> export_entries() const;

  // --- shard introspection (diagnostics and tests) --------------------------

  std::size_t shard_count() const noexcept { return shards_.size(); }

  /// Which shard a key belongs to. Deterministic for a given shard count.
  std::size_t shard_index_for(const std::string& key) const;

  /// Direct access to one shard. For tests and diagnostics only -- the network
  /// layer never needs this and never uses it.
  SyncCache& shard_at(std::size_t index) { return *shards_[index]; }
  const SyncCache& shard_at(std::size_t index) const { return *shards_[index]; }

 private:
  SyncCache& shard_for(const std::string& key) {
    return *shards_[shard_index_for(key)];
  }

  // unique_ptr because SyncCache owns a mutex and so is neither copyable nor
  // movable -- it cannot live in a vector directly.
  std::vector<std::unique_ptr<SyncCache>> shards_;
};

/// The shard-selection hash, exposed for testing.
///
/// This deliberately is *not* std::hash on its own. std::unordered_map already
/// hashes the key with std::hash to pick a bucket; if the shard index used the
/// same value, every key in a shard would share `h % shard_count`, which
/// correlates with the bucket index whenever the shard count and bucket count
/// share a factor -- clustering keys into a few buckets of each shard's map.
/// Running the hash through a mixing step decorrelates the two uses.
std::uint64_t shard_hash(const std::string& key);

}  // namespace cachex
