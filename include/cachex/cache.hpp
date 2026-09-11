#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "cachex/recency_list.hpp"

namespace cachex {

/// The three states a key's time-to-live can be in.
///
/// Redis encodes these in one integer with two magic values (-2 missing, -1 no
/// expiry, >= 0 remaining). A type makes the three cases explicit instead, so a
/// caller cannot accidentally treat "missing" as a duration.
enum class TtlState {
  Missing,     ///< No such key: never set, or erased, evicted, or expired.
  Persistent,  ///< The key exists and will not expire on its own.
  Expiring,    ///< The key exists and has a deadline.
};

struct TtlInfo {
  TtlState state = TtlState::Missing;
  /// Time left before expiry. Only meaningful when state == Expiring; zero
  /// otherwise. Truncated toward zero, so a key with 0.4 ms left reports 0 ms
  /// while still being Expiring.
  std::chrono::milliseconds remaining{0};
};

/// One entry as an outside observer sees it: no iterators, no deadlines tied to
/// this process, nothing private.
///
/// The TTL is a *remaining duration*, not the absolute deadline the entry stores
/// internally. That is forced by the clock choice: steady_clock's epoch is
/// unspecified (in practice, boot time), so a deadline from one process is
/// meaningless in the next one. Converting to "time left" at export is the price
/// of using a monotonic clock, and it was flagged as future work back in §6.2.
struct EntrySnapshot {
  std::string key;
  std::string value;
  /// nullopt means the entry never expires.
  std::optional<std::chrono::milliseconds> remaining_ttl;
};

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
/// **Not thread-safe.** This class is deliberately single-threaded: it pays for
/// no locking and the in-process benchmarks measure the data structure rather
/// than a mutex. Wrap it in SyncCache to share it between threads -- and note
/// that get() mutates (it reorders the recency list), so concurrent "reads"
/// need exclusive access just as writes do. See sync_cache.hpp.
class Cache {
 public:
  /// Unbounded: nothing is ever evicted and memory grows with the key count.
  Cache() = default;

  /// Bounded: once `capacity` entries are stored, inserting a new key evicts the
  /// least recently used one. A capacity of 0 is legal and stores nothing.
  explicit Cache(std::size_t capacity);

  using Clock = Entry::Clock;
  using Duration = std::chrono::milliseconds;

  /// Inserts a new entry or overwrites an existing one, and marks the key as most
  /// recently used. Inserting at capacity evicts the least recently used entry.
  /// The entry never expires; if the key already had a TTL, **this clears it**
  /// -- a set replaces the whole entry, expiry included, as Redis's SET does.
  /// O(1) average, including the eviction.
  void set(std::string key, std::string value);

  /// As above, but the entry expires `ttl` from now.
  ///
  /// A `ttl` of zero or less **erases the key and stores nothing**. The entry
  /// could never be read, and storing it would occupy capacity and could evict a
  /// live entry. The invariant that falls out: after set(k, v, ttl), the key is
  /// visible if and only if ttl > 0.
  ///
  /// Accepts any coarser chrono duration implicitly, so set(k, v, seconds(30))
  /// compiles. O(1) average.
  void set(std::string key, std::string value, Duration ttl);

  /// Reports whether the key exists, never expires, or expires -- and if so, how
  /// long is left. Expired entries are reclaimed on the way through, which is
  /// why this is not const. Does *not* count as a use: querying metadata should
  /// not rescue a key from eviction. O(1) average.
  TtlInfo ttl(const std::string& key);

  /// Returns a *copy* of the value, or nullopt if the key is absent.
  /// A hit counts as a use and reorders the entry -- which is why it is not const.
  /// O(1) average.
  std::optional<std::string> get(const std::string& key);

  /// Removes a key. Returns false if it was not present.
  /// An expired-but-not-yet-reclaimed entry counts as not present: it is
  /// reclaimed, and the call returns false, because nothing user-visible was
  /// removed. O(1) average.
  bool erase(const std::string& key);

  /// Tests for a key *without* counting as a use, so recency order is unchanged
  /// and the method can be const. A key that is only ever peeked at still ages
  /// out.
  ///
  /// An expired entry reports false but is **not** reclaimed -- reclaiming would
  /// mutate, and this is the one deliberately non-mutating peek. Reclamation is
  /// left to the next get() or ttl(). O(1) average.
  bool contains(const std::string& key) const;

  /// Number of entries *resident*, which under lazy expiration includes expired
  /// entries not yet reclaimed. It is therefore an upper bound on the number of
  /// visible keys, not a count of them. Redis's DBSIZE behaves the same way.
  /// Counting only live keys would mean scanning -- O(n). O(1).
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

  /// Entries removed because they were found expired, cumulative over the
  /// cache's lifetime. Like evictions(), not reset by clear().
  std::size_t expired_removals() const noexcept { return expired_removals_; }

  /// Every live entry, most recently used first.
  ///
  /// Expired entries are skipped rather than reclaimed -- this is const, like
  /// contains(). O(n), and it copies everything, so it is for persistence and
  /// diagnostics, not for the request path.
  std::vector<EntrySnapshot> export_entries() const;

  /// Keys from most to least recently used -- so `.back()` is the next victim.
  /// Includes expired entries that have not been reclaimed yet.
  /// Diagnostic only: O(n) and allocates. The server never calls it.
  std::vector<std::string> keys_by_recency() const;

 private:
  using Index = std::unordered_map<std::string, RecencyList::Iterator>;

  /// The shared body of both set() overloads.
  void store(std::string key, std::string value,
             std::optional<Clock::time_point> expires_at);

  /// Removes an entry from both structures, in the only safe order. O(1).
  void remove(Index::iterator it);

  /// Removes the least recently used entry from both structures. O(1).
  void evict_oldest();

  // entries_ owns the data; index_ holds non-owning iterators into it.
  // Declared in this order so that on destruction index_ (the borrower) is
  // destroyed before entries_ (the owner).
  RecencyList entries_;
  Index index_;

  std::optional<std::size_t> capacity_;
  std::size_t evictions_ = 0;
  std::size_t expired_removals_ = 0;
};

}  // namespace cachex
