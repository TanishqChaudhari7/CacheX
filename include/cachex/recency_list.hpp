#pragma once

#include <chrono>
#include <cstddef>
#include <list>
#include <optional>
#include <string>

namespace cachex {

/// One cached key-value pair.
///
/// The key is stored here *as well as* in the Cache's hash map. That duplication
/// is deliberate: eviction walks from the list to the map (take the oldest node,
/// then remove its key from the index), and without the key in the node there
/// would be no way to find the map entry to erase. See ARCHITECTURE.md §8.
struct Entry {
  /// steady_clock, not system_clock: it is monotonic, so an NTP correction or a
  /// manual clock change cannot resurrect an expired key or mass-expire live
  /// ones. See ARCHITECTURE.md §6.2.
  using Clock = std::chrono::steady_clock;

  std::string key;
  std::string value;

  /// Absolute deadline, not a duration: "is this expired?" is then a single
  /// comparison against now(), with nothing to re-base against a start time.
  /// nullopt means the entry never expires on its own.
  ///
  /// Defaulted so `Entry{"k", "v"}` still means "no expiry".
  std::optional<Clock::time_point> expires_at{};
};

/// The recency ordering: most recently used at the front, least at the back.
///
/// This is a thin layer over std::list, chosen for one property: splicing a node
/// within a list is O(1) and does not invalidate iterators or references to it.
/// That is what lets the Cache hold an iterator per key in its hash map and still
/// reorder entries freely.
///
/// The list owns every Entry. Everything else holds non-owning iterators into it.
class RecencyList {
 public:
  using List = std::list<Entry>;
  using Iterator = List::iterator;
  using ConstIterator = List::const_iterator;

  /// Inserts a new entry at the front (most recently used). O(1).
  /// The returned iterator stays valid until that entry is erased.
  /// Takes a whole Entry so that adding fields to it does not change this
  /// signature -- which is what happened when TTL arrived.
  Iterator insert_newest(Entry entry);

  /// Moves an existing entry to the front. O(1), no allocation, and `it` remains
  /// valid afterwards -- splice relinks nodes rather than moving their contents.
  void mark_used(Iterator it);

  /// Removes an entry. O(1). Invalidates only `it`.
  void erase(Iterator it);

  /// The least recently used entry -- the eviction candidate. O(1).
  /// Undefined if the list is empty.
  const Entry& oldest() const;

  /// Removes the least recently used entry. O(1). Undefined if empty.
  void pop_oldest();

  void clear() noexcept;

  std::size_t size() const noexcept { return entries_.size(); }
  bool empty() const noexcept { return entries_.empty(); }

  /// Front-to-back (most recent first). For iteration in tests and diagnostics.
  ConstIterator begin() const noexcept { return entries_.begin(); }
  ConstIterator end() const noexcept { return entries_.end(); }

 private:
  List entries_;
};

}  // namespace cachex
