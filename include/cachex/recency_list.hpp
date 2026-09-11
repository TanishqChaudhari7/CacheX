#pragma once

#include <cstddef>
#include <list>
#include <string>

namespace cachex {

/// One cached key-value pair.
///
/// The key is stored here *as well as* in the Cache's hash map. That duplication
/// is deliberate: eviction walks from the list to the map (take the oldest node,
/// then remove its key from the index), and without the key in the node there
/// would be no way to find the map entry to erase. See ARCHITECTURE.md §6.
struct Entry {
  std::string key;
  std::string value;
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
  Iterator insert_newest(std::string key, std::string value);

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
