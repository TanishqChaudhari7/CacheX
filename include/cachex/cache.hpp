#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "cachex/recency_list.hpp"

namespace cachex {

/// A single-threaded, unbounded in-memory key-value cache.
///
/// Two structures cooperate:
///
///   index_   unordered_map<key, iterator>  -- answers "where is this key?" in O(1)
///   entries_ RecencyList (std::list<Entry>) -- owns the data, ordered most to
///                                              least recently used
///
/// Neither can do the other's job: a hash map has no ordering, and a list cannot
/// search. Combining them gives O(1) lookup *and* O(1) recency updates, which is
/// what O(1) LRU eviction will need in Stage 3.
///
/// Not thread-safe. Every method may be called only from one thread at a time;
/// locking arrives in Stage 8.
///
/// Unbounded: nothing is evicted yet, so memory grows with the number of keys.
class Cache {
 public:
  Cache() = default;

  /// Inserts a new entry or overwrites an existing one, and marks the key as
  /// most recently used. O(1) average.
  void set(std::string key, std::string value);

  /// Returns a *copy* of the value, or nullopt if the key is absent.
  /// Counts as a use, so it reorders the entry -- which is why it is not const.
  /// O(1) average.
  std::optional<std::string> get(const std::string& key);

  /// Removes a key. Returns false if it was not present. O(1) average.
  bool erase(const std::string& key);

  /// Tests for a key *without* counting as a use, so recency order is unchanged
  /// and the method can be const. O(1) average.
  bool contains(const std::string& key) const;

  /// Number of entries. O(1).
  std::size_t size() const noexcept { return index_.size(); }
  bool empty() const noexcept { return index_.empty(); }

  /// Removes every entry. O(n).
  void clear() noexcept;

  /// Keys from most to least recently used.
  /// Diagnostic only -- it is O(n) and allocates; the server never calls it.
  /// It exists so the recency invariant is testable before eviction makes it
  /// externally visible in Stage 3.
  std::vector<std::string> keys_by_recency() const;

 private:
  // entries_ owns the data; index_ holds non-owning iterators into it.
  // Declared in this order so that on destruction index_ (the borrower) is
  // destroyed before entries_ (the owner).
  RecencyList entries_;
  std::unordered_map<std::string, RecencyList::Iterator> index_;
};

}  // namespace cachex
