#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "cachex/recency_list.hpp"

namespace cachex {

/// A single-threaded in-memory key-value cache with optional LRU eviction.
///
/// Two structures cooperate:
///
///   index_   unordered_map<key, iterator>   -- answers "where is this key?" O(1)
///   entries_ RecencyList (std::list<Entry>) -- owns the data, ordered most to
///                                              least recently used
///
/// Neither can do the other's job: a hash map has no ordering, and a list cannot
/// search. Together they give O(1) lookup *and* O(1) recency updates *and* O(1)
/// access to the eviction candidate -- which is what makes LRU O(1).
///
/// Not thread-safe. Locking arrives in Stage 8.
class Cache {
 public:
  /// Unbounded: nothing is ever evicted and memory grows with the key count.
  Cache() = default;

  /// Bounded: once `capacity` entries are stored, inserting a new key evicts the
  /// least recently used one. A capacity of 0 is legal and stores nothing.
  explicit Cache(std::size_t capacity);

  /// Inserts a new entry or overwrites an existing one, and marks the key as most
  /// recently used. Inserting at capacity evicts the least recently used entry.
  /// O(1) average, including the eviction.
  void set(std::string key, std::string value);

  /// Returns a *copy* of the value, or nullopt if the key is absent.
  /// A hit counts as a use and reorders the entry -- which is why it is not const.
  /// O(1) average.
  std::optional<std::string> get(const std::string& key);

  /// Removes a key. Returns false if it was not present. O(1) average.
  bool erase(const std::string& key);

  /// Tests for a key *without* counting as a use, so recency order is unchanged
  /// and the method can be const. A key that is only ever peeked at still ages
  /// out. O(1) average.
  bool contains(const std::string& key) const;

  /// Number of entries currently stored. Never exceeds capacity(). O(1).
  std::size_t size() const noexcept { return index_.size(); }
  bool empty() const noexcept { return index_.empty(); }

  /// Removes every entry. Capacity and the eviction counter are unaffected. O(n).
  void clear() noexcept;

  /// nullopt when the cache is unbounded.
  std::optional<std::size_t> capacity() const noexcept { return capacity_; }

  /// Entries evicted to stay within capacity, cumulative over the cache's
  /// lifetime. Deliberately *not* reset by clear(), matching how servers report
  /// lifetime statistics. Explicit erase() is not an eviction and is not counted.
  std::size_t evictions() const noexcept { return evictions_; }

  /// Keys from most to least recently used -- so `.back()` is the next victim.
  /// Diagnostic only: O(n) and allocates. The server never calls it.
  std::vector<std::string> keys_by_recency() const;

 private:
  /// Removes the least recently used entry from both structures. O(1).
  void evict_oldest();

  // entries_ owns the data; index_ holds non-owning iterators into it.
  // Declared in this order so that on destruction index_ (the borrower) is
  // destroyed before entries_ (the owner).
  RecencyList entries_;
  std::unordered_map<std::string, RecencyList::Iterator> index_;

  std::optional<std::size_t> capacity_;
  std::size_t evictions_ = 0;
};

}  // namespace cachex
