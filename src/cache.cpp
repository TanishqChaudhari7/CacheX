#include "cachex/cache.hpp"

#include <utility>

namespace cachex {

void Cache::set(std::string key, std::string value) {
  if (const auto it = index_.find(key); it != index_.end()) {
    // Overwrite in place. The node keeps its identity, so every other iterator
    // -- including the one stored in the map -- stays valid.
    it->second->value = std::move(value);
    entries_.mark_used(it->second);
    return;
  }

  // The key is needed in two places, so exactly one copy is made: the list node
  // copies it here, and the map then takes ownership of the original by move.
  const RecencyList::Iterator node = entries_.insert_newest(key, std::move(value));
  index_.emplace(std::move(key), node);
}

std::optional<std::string> Cache::get(const std::string& key) {
  const auto it = index_.find(key);
  if (it == index_.end()) {
    return std::nullopt;
  }
  entries_.mark_used(it->second);

  // A copy, not a reference. A reference would dangle the moment the caller made
  // another call that erased or (from Stage 3) evicted this entry.
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
