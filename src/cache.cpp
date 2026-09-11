#include "cachex/cache.hpp"

#include <utility>

namespace cachex {
namespace {

/// Whether an entry's deadline has passed.
///
/// The short-circuit matters: an entry with no deadline never reads the clock.
/// steady_clock::now() costs roughly as much as the hash lookup itself, so
/// checking `has_value()` first is what keeps TTL close to free for the keys
/// that do not use it. Benchmarked in ARCHITECTURE.md §9.
bool is_expired(const Entry& entry) {
  return entry.expires_at.has_value() && *entry.expires_at <= Entry::Clock::now();
}

}  // namespace

Cache::Cache(std::size_t capacity) : capacity_(capacity) {}

void Cache::set(std::string key, std::string value) {
  store(std::move(key), std::move(value), std::nullopt);
}

void Cache::set(std::string key, std::string value, Duration ttl) {
  if (ttl <= Duration::zero()) {
    // The key must not be visible after this call, and an entry that can never
    // be read is pure cost -- it would occupy capacity and could evict a live
    // entry. So erase rather than store something born dead.
    erase(key);
    return;
  }
  store(std::move(key), std::move(value), Clock::now() + ttl);
}

void Cache::store(std::string key, std::string value,
                  std::optional<Clock::time_point> expires_at) {
  if (const auto it = index_.find(key); it != index_.end()) {
    // Overwrite in place, expiry included: a set replaces the whole entry, so
    // set(k, v) on a key that had a TTL makes it persistent again. The node
    // keeps its identity, so every iterator -- including the map's -- stays
    // valid. This is also the path taken for an *expired* entry: a set revives
    // the key rather than treating it as absent.
    it->second->value = std::move(value);
    it->second->expires_at = expires_at;
    entries_.mark_used(it->second);
    // An update cannot exceed capacity: the entry count is unchanged. Writing to
    // a key counts as using it, so the entry moves to the front -- see the
    // recency-semantics note in ARCHITECTURE.md §4.6.
    return;
  }

  // The key is needed in two places, so exactly one copy is made: the list node
  // copies it here, and the map then takes ownership of the original by move.
  const RecencyList::Iterator node =
      entries_.insert_newest(Entry{key, std::move(value), expires_at});
  index_.emplace(std::move(key), node);

  // Insert first, then trim back to capacity. The new entry is at the front, so
  // it is never its own victim -- except at capacity 0, where nothing can be
  // stored at all and the loop removes what was just added.
  //
  // `while` rather than `if` so that a capacity of 0 and any future
  // lower-the-capacity operation both terminate correctly. In the steady state
  // it runs at most once, so set() stays O(1) average.
  while (capacity_.has_value() && index_.size() > *capacity_) {
    evict_oldest();
  }
}

void Cache::remove(Index::iterator it) {
  // Order matters: it->second is read to find the list node, so the map entry
  // holding it must be erased second.
  entries_.erase(it->second);
  index_.erase(it);
}

void Cache::evict_oldest() {
  // This is the payoff for storing the key inside the Entry. Eviction runs
  // list -> map: the list knows which entry is oldest, but removing it from the
  // index needs its key. Without the key here the only way to find the index
  // entry would be to scan the whole map -- O(n), which would defeat the point.
  const std::string& victim_key = entries_.oldest().key;

  // Safe ordering: erasing from the map does not touch the list, so victim_key
  // (a reference into the list node) stays valid until pop_oldest() destroys it.
  index_.erase(victim_key);
  entries_.pop_oldest();
  ++evictions_;
}

std::optional<std::string> Cache::get(const std::string& key) {
  const auto it = index_.find(key);
  if (it == index_.end()) {
    return std::nullopt;
  }

  // Lazy expiration: an expired entry is indistinguishable from an absent one to
  // the caller, and this is the moment we happen to be holding it, so reclaim it
  // now rather than leaving it to a sweep that does not exist yet.
  if (is_expired(*it->second)) {
    remove(it);
    ++expired_removals_;
    return std::nullopt;
  }

  entries_.mark_used(it->second);

  // A copy, not a reference. A reference would dangle the moment the caller made
  // another call that erased or evicted this entry -- and now that eviction
  // exists, *any* set() on a full cache can be that call.
  return it->second->value;
}

TtlInfo Cache::ttl(const std::string& key) {
  const auto it = index_.find(key);
  if (it == index_.end()) {
    return TtlInfo{TtlState::Missing, Duration::zero()};
  }

  const Entry& entry = *it->second;
  if (!entry.expires_at.has_value()) {
    return TtlInfo{TtlState::Persistent, Duration::zero()};
  }

  // now() is sampled once and reused, so the answer cannot contradict itself:
  // reading the clock twice could report Expiring with a negative remainder.
  const Clock::time_point now = Clock::now();
  if (*entry.expires_at <= now) {
    remove(it);
    ++expired_removals_;
    return TtlInfo{TtlState::Missing, Duration::zero()};
  }

  return TtlInfo{TtlState::Expiring,
                 std::chrono::duration_cast<Duration>(*entry.expires_at - now)};
}

bool Cache::erase(const std::string& key) {
  const auto it = index_.find(key);
  if (it == index_.end()) {
    return false;
  }

  // An expired entry is already logically gone, so reclaim it but report false:
  // the caller did not remove anything that was visible.
  const bool was_visible = !is_expired(*it->second);
  if (!was_visible) {
    ++expired_removals_;
  }
  remove(it);
  return was_visible;
}

bool Cache::contains(const std::string& key) const {
  const auto it = index_.find(key);
  return it != index_.end() && !is_expired(*it->second);
}

void Cache::clear() noexcept {
  index_.clear();
  entries_.clear();
}

std::vector<std::string> Cache::keys_by_recency() const {
  std::vector<std::string> keys;
  keys.reserve(entries_.size());
  for (const Entry& entry : entries_) {
    keys.push_back(entry.key);
  }
  return keys;
}

}  // namespace cachex
