#include "cachex/cache.hpp"

#include <utility>

namespace cachex {

Cache::Cache(std::size_t capacity) : capacity_(capacity) {}

void Cache::set(std::string key, std::string value) {
  if (const auto it = index_.find(key); it != index_.end()) {
    // Overwrite in place. The node keeps its identity, so every other iterator
    // -- including the one stored in the map -- stays valid.
    it->second->value = std::move(value);
    entries_.mark_used(it->second);
    // An update cannot exceed capacity: the entry count is unchanged. Writing to
    // a key counts as using it, so the entry moves to the front -- see the
    // recency-semantics note in ARCHITECTURE.md §4.6.
    return;
  }

  // The key is needed in two places, so exactly one copy is made: the list node
  // copies it here, and the map then takes ownership of the original by move.
  const RecencyList::Iterator node = entries_.insert_newest(key, std::move(value));
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
  entries_.mark_used(it->second);

  // A copy, not a reference. A reference would dangle the moment the caller made
  // another call that erased or evicted this entry -- and now that eviction
  // exists, *any* set() on a full cache can be that call.
  return it->second->value;
}

bool Cache::erase(const std::string& key) {
  const auto it = index_.find(key);
  if (it == index_.end()) {
    return false;
  }

  // Order matters: it->second is read to find the list node, so the map entry
  // holding it must be erased second.
  entries_.erase(it->second);
  index_.erase(it);
  return true;
}

bool Cache::contains(const std::string& key) const {
  return index_.find(key) != index_.end();
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
