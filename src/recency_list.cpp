#include "cachex/recency_list.hpp"

#include <utility>

namespace cachex {

RecencyList::Iterator RecencyList::insert_newest(std::string key,
                                                 std::string value) {
  entries_.push_front(Entry{std::move(key), std::move(value)});
  return entries_.begin();
}

void RecencyList::mark_used(Iterator it) {
  // Splice moves the node itself -- no Entry is copied, no memory is allocated,
  // and `it` still points at the same node afterwards. The standard defines this
  // as a no-op when the node is already at the requested position, so touching
  // the front element needs no special case.
  entries_.splice(entries_.begin(), entries_, it);
}

void RecencyList::erase(Iterator it) { entries_.erase(it); }

const Entry& RecencyList::oldest() const { return entries_.back(); }

void RecencyList::pop_oldest() { entries_.pop_back(); }

void RecencyList::clear() noexcept { entries_.clear(); }

}  // namespace cachex
