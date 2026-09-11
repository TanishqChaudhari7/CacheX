#include "cachex/sharded_cache.hpp"

#include <algorithm>
#include <functional>
#include <utility>

namespace cachex {

std::uint64_t shard_hash(const std::string& key) {
  std::uint64_t h = static_cast<std::uint64_t>(std::hash<std::string>{}(key));

  // splitmix64's finalizer: an avalanche step that spreads every input bit over
  // all 64 output bits. Cheap (three shifts, two multiplies) and it removes the
  // correlation with the hash the shard's own unordered_map will compute.
  h ^= h >> 30;
  h *= 0xbf58476d1ce4e5b9ULL;
  h ^= h >> 27;
  h *= 0x94d049bb133111ebULL;
  h ^= h >> 31;
  return h;
}

namespace {

std::size_t sane_shard_count(std::size_t requested) {
  return requested == 0 ? 1 : requested;
}

}  // namespace

ShardedCache::ShardedCache(std::size_t shard_count) {
  const std::size_t count = sane_shard_count(shard_count);
  shards_.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    shards_.push_back(std::make_unique<SyncCache>());
  }
}

ShardedCache::ShardedCache(std::size_t shard_count, std::size_t total_capacity) {
  // Never leave a shard with capacity 0: it would accept no keys at all, and
  // every key hashing to it would vanish silently.
  std::size_t count = sane_shard_count(shard_count);
  if (total_capacity < count) {
    count = std::max<std::size_t>(1, total_capacity);
  }

  // Split as evenly as possible, handing the remainder to the first few shards
  // so the parts sum to exactly total_capacity rather than to a rounded-down
  // approximation of it.
  const std::size_t base = total_capacity / count;
  const std::size_t remainder = total_capacity % count;

  shards_.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    shards_.push_back(
        std::make_unique<SyncCache>(base + (i < remainder ? 1 : 0)));
  }
}

std::size_t ShardedCache::shard_index_for(const std::string& key) const {
  return static_cast<std::size_t>(shard_hash(key) % shards_.size());
}

void ShardedCache::set(std::string key, std::string value) {
  // The shard is chosen before the lock is taken, and only that one shard is
  // locked. Two threads writing keys in different shards never meet.
  SyncCache& shard = shard_for(key);
  shard.set(std::move(key), std::move(value));
}

void ShardedCache::set(std::string key, std::string value, Duration ttl) {
  SyncCache& shard = shard_for(key);
  shard.set(std::move(key), std::move(value), ttl);
}

std::optional<std::string> ShardedCache::get(const std::string& key) {
  return shard_for(key).get(key);
}

bool ShardedCache::erase(const std::string& key) {
  return shard_for(key).erase(key);
}

bool ShardedCache::contains(const std::string& key) {
  return shard_for(key).contains(key);
}

TtlInfo ShardedCache::ttl(const std::string& key) { return shard_for(key).ttl(key); }

void ShardedCache::clear() {
  for (const std::unique_ptr<SyncCache>& shard : shards_) {
    shard->clear();
  }
}

std::size_t ShardedCache::size() const {
  std::size_t total = 0;
  for (const std::unique_ptr<SyncCache>& shard : shards_) {
    total += shard->size();
  }
  return total;
}

bool ShardedCache::empty() const { return size() == 0; }

std::optional<std::size_t> ShardedCache::capacity() const {
  if (!shards_.front()->capacity().has_value()) {
    return std::nullopt;  // all shards are created alike
  }
  std::size_t total = 0;
  for (const std::unique_ptr<SyncCache>& shard : shards_) {
    total += shard->capacity().value_or(0);
  }
  return total;
}

std::size_t ShardedCache::evictions() const {
  std::size_t total = 0;
  for (const std::unique_ptr<SyncCache>& shard : shards_) {
    total += shard->evictions();
  }
  return total;
}

std::size_t ShardedCache::expired_removals() const {
  std::size_t total = 0;
  for (const std::unique_ptr<SyncCache>& shard : shards_) {
    total += shard->expired_removals();
  }
  return total;
}

}  // namespace cachex
