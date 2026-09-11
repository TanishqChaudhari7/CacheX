#include "cachex/cache.hpp"

#include <chrono>
#include <string>
#include <thread>

#include "test_framework.hpp"

namespace {

using namespace std::chrono_literals;

// Timing tolerances. Sleeps are guaranteed to last *at least* the requested
// duration but may overshoot, so the safe design is:
//
//   * "still alive" assertions use a TTL far longer than any plausible stall,
//     since only an enormous pause could falsify them;
//   * "now expired" assertions wait several times the TTL, since overshooting
//     only makes them more true.
//
// Both directions therefore fail only if the machine misbehaves by an order of
// magnitude, not by a few milliseconds.
constexpr auto kLongTtl = 2000ms;       // will not expire during a test
constexpr auto kShortTtl = 40ms;        // will expire promptly
constexpr auto kWaitPastShort = 130ms;  // >3x kShortTtl

// Note the asymmetry in how those are sized. kWaitPastShort only has to exceed
// kShortTtl, because a sleep can overshoot but never undershoot -- so no amount
// of system load can make an expired key look alive. kShortTtl itself is the
// value that must stay generous: a few tests assert a key is alive immediately
// after setting it, and that is the one assertion a badly stalled machine could
// break. Hence a small wait and a comparatively large TTL, rather than the
// reverse.

std::string state_name(cachex::TtlState state) {
  switch (state) {
    case cachex::TtlState::Missing:
      return "Missing";
    case cachex::TtlState::Persistent:
      return "Persistent";
    case cachex::TtlState::Expiring:
      return "Expiring";
  }
  return "<invalid>";
}

}  // namespace

// --- no TTL ----------------------------------------------------------------

CACHEX_TEST(a_key_set_without_ttl_is_persistent) {
  cachex::Cache cache;
  cache.set("k", "v");

  CHECK_EQ(state_name(cache.ttl("k").state), "Persistent");
  CHECK_EQ(cache.ttl("k").remaining.count(), 0);
}

CACHEX_TEST(a_persistent_key_survives_a_wait) {
  cachex::Cache cache;
  cache.set("k", "v");

  std::this_thread::sleep_for(kWaitPastShort);

  CHECK_EQ(cache.get("k").value_or(""), "v");
  CHECK(cache.contains("k"));
  CHECK_EQ(state_name(cache.ttl("k").state), "Persistent");
}

CACHEX_TEST(ttl_of_a_missing_key_is_missing) {
  cachex::Cache cache;
  CHECK_EQ(state_name(cache.ttl("never-set").state), "Missing");
  CHECK_EQ(cache.ttl("never-set").remaining.count(), 0);
  CHECK_EQ(cache.size(), 0u);  // querying must not insert
}

CACHEX_TEST(ttl_of_an_erased_key_is_missing) {
  cachex::Cache cache;
  cache.set("k", "v", kLongTtl);
  cache.erase("k");

  CHECK_EQ(state_name(cache.ttl("k").state), "Missing");
}

// --- positive TTL, before expiry -------------------------------------------

CACHEX_TEST(a_key_with_a_ttl_reports_expiring) {
  cachex::Cache cache;
  cache.set("k", "v", kLongTtl);

  const cachex::TtlInfo info = cache.ttl("k");
  CHECK_EQ(state_name(info.state), "Expiring");
  CHECK(info.remaining.count() > 0);
  // Never more than what was asked for: the deadline is now + ttl, and time
  // only moves forward between the set and the query.
  CHECK(info.remaining <= kLongTtl);
}

CACHEX_TEST(a_key_is_readable_before_it_expires) {
  cachex::Cache cache;
  cache.set("k", "v", kLongTtl);

  CHECK_EQ(cache.get("k").value_or(""), "v");
  CHECK(cache.contains("k"));
  CHECK_EQ(cache.size(), 1u);
  CHECK_EQ(cache.expired_removals(), 0u);
}

CACHEX_TEST(remaining_ttl_decreases_over_time) {
  cachex::Cache cache;
  cache.set("k", "v", kLongTtl);

  const auto first = cache.ttl("k").remaining;
  std::this_thread::sleep_for(120ms);
  const auto second = cache.ttl("k").remaining;

  CHECK(second < first);
  // The sleep guarantees at least 120 ms elapsed; assert a conservative 50 ms
  // so a coarse clock or a slow wake-up cannot fail the test.
  CHECK((first - second) >= 50ms);
  CHECK(second.count() > 0);  // 2000 ms TTL, ~120 ms elapsed: still alive
  CHECK_EQ(state_name(cache.ttl("k").state), "Expiring");
}

// --- after expiry ----------------------------------------------------------

CACHEX_TEST(an_expired_key_reads_as_missing) {
  cachex::Cache cache;
  cache.set("k", "v", kShortTtl);
  CHECK(cache.get("k").has_value());  // alive immediately after the set

  std::this_thread::sleep_for(kWaitPastShort);

  CHECK(!cache.get("k").has_value());
  CHECK(!cache.contains("k"));
  CHECK_EQ(state_name(cache.ttl("k").state), "Missing");
}

CACHEX_TEST(getting_an_expired_key_reclaims_it) {
  cachex::Cache cache;
  cache.set("k", "v", kShortTtl);
  std::this_thread::sleep_for(kWaitPastShort);

  // Still resident: lazy expiration has not been given a chance to run yet.
  CHECK_EQ(cache.size(), 1u);
  CHECK_EQ(cache.expired_removals(), 0u);

  CHECK(!cache.get("k").has_value());

  CHECK_EQ(cache.size(), 0u);
  CHECK_EQ(cache.expired_removals(), 1u);
}

CACHEX_TEST(querying_ttl_of_an_expired_key_reclaims_it) {
  cachex::Cache cache;
  cache.set("k", "v", kShortTtl);
  std::this_thread::sleep_for(kWaitPastShort);

  CHECK_EQ(state_name(cache.ttl("k").state), "Missing");
  CHECK_EQ(cache.size(), 0u);
  CHECK_EQ(cache.expired_removals(), 1u);
}

// This is the documented limitation of lazy expiration, pinned as a test so it
// cannot change silently: contains() is the one deliberately non-mutating peek,
// so it reports the truth but leaves the body behind.
CACHEX_TEST(contains_reports_expiry_without_reclaiming) {
  cachex::Cache cache;
  cache.set("k", "v", kShortTtl);
  std::this_thread::sleep_for(kWaitPastShort);

  CHECK(!cache.contains("k"));
  CHECK_EQ(cache.size(), 1u);            // still occupying a slot
  CHECK_EQ(cache.expired_removals(), 0u);

  cache.get("k");                        // a mutating call does reclaim it
  CHECK_EQ(cache.size(), 0u);
}

CACHEX_TEST(erasing_an_expired_key_reports_false_but_reclaims_it) {
  cachex::Cache cache;
  cache.set("k", "v", kShortTtl);
  std::this_thread::sleep_for(kWaitPastShort);

  // Nothing user-visible was removed, so false -- but the entry is gone.
  CHECK(!cache.erase("k"));
  CHECK_EQ(cache.size(), 0u);
  CHECK_EQ(cache.expired_removals(), 1u);
}

CACHEX_TEST(an_expired_key_can_be_set_again) {
  cachex::Cache cache;
  cache.set("k", "old", kShortTtl);
  std::this_thread::sleep_for(kWaitPastShort);

  cache.set("k", "new", kLongTtl);

  CHECK_EQ(cache.get("k").value_or(""), "new");
  CHECK_EQ(cache.size(), 1u);
  CHECK_EQ(state_name(cache.ttl("k").state), "Expiring");
}

// --- updating TTL ----------------------------------------------------------

CACHEX_TEST(setting_with_a_new_ttl_replaces_the_old_one) {
  cachex::Cache cache;
  cache.set("k", "v", kShortTtl);

  cache.set("k", "v", kLongTtl);  // extend before the short TTL lands
  std::this_thread::sleep_for(kWaitPastShort);

  CHECK_EQ(cache.get("k").value_or(""), "v");
  CHECK_EQ(state_name(cache.ttl("k").state), "Expiring");
}

CACHEX_TEST(setting_without_a_ttl_clears_an_existing_ttl) {
  cachex::Cache cache;
  cache.set("k", "v", kShortTtl);
  CHECK_EQ(state_name(cache.ttl("k").state), "Expiring");

  // A set replaces the whole entry, expiry included -- matching Redis's SET.
  cache.set("k", "v");
  CHECK_EQ(state_name(cache.ttl("k").state), "Persistent");

  std::this_thread::sleep_for(kWaitPastShort);
  CHECK_EQ(cache.get("k").value_or(""), "v");
}

CACHEX_TEST(shortening_a_ttl_works_too) {
  cachex::Cache cache;
  cache.set("k", "v", kLongTtl);
  cache.set("k", "v", kShortTtl);

  std::this_thread::sleep_for(kWaitPastShort);
  CHECK(!cache.get("k").has_value());
}

CACHEX_TEST(updating_a_ttl_does_not_change_the_entry_count) {
  cachex::Cache cache;
  cache.set("k", "v", kShortTtl);
  cache.set("k", "v", kLongTtl);
  cache.set("k", "v");

  CHECK_EQ(cache.size(), 1u);
}

// --- TTL <= 0 --------------------------------------------------------------

CACHEX_TEST(a_zero_ttl_stores_nothing) {
  cachex::Cache cache;
  cache.set("k", "v", 0ms);

  CHECK_EQ(cache.size(), 0u);
  CHECK(!cache.contains("k"));
  CHECK(!cache.get("k").has_value());
  CHECK_EQ(state_name(cache.ttl("k").state), "Missing");
}

CACHEX_TEST(a_negative_ttl_stores_nothing) {
  cachex::Cache cache;
  cache.set("k", "v", -5000ms);

  CHECK_EQ(cache.size(), 0u);
  CHECK(!cache.contains("k"));
}

CACHEX_TEST(a_zero_ttl_erases_an_existing_key) {
  cachex::Cache cache;
  cache.set("k", "v");
  CHECK(cache.contains("k"));

  // The invariant: after set(k, v, ttl) the key is visible iff ttl > 0.
  cache.set("k", "v", 0ms);

  CHECK(!cache.contains("k"));
  CHECK_EQ(cache.size(), 0u);
}

CACHEX_TEST(a_zero_ttl_on_a_missing_key_is_a_no_op) {
  cachex::Cache cache;
  cache.set("absent", "v", 0ms);

  CHECK_EQ(cache.size(), 0u);
  CHECK_EQ(cache.expired_removals(), 0u);
}

CACHEX_TEST(a_one_millisecond_ttl_is_a_real_ttl_not_a_delete) {
  cachex::Cache cache;
  cache.set("k", "v", 1ms);

  // The boundary is strictly at zero: 1 ms is stored, and expires normally.
  CHECK_EQ(cache.size(), 1u);
  std::this_thread::sleep_for(kWaitPastShort);
  CHECK(!cache.get("k").has_value());
}

// --- interaction with recency and capacity ---------------------------------

CACHEX_TEST(querying_ttl_does_not_count_as_a_use) {
  cachex::Cache cache(3);
  cache.set("a", "1");
  cache.set("b", "2");
  cache.set("c", "3");

  cache.ttl("a");  // metadata query, not a use -- must not rescue "a"
  cache.set("d", "4");

  CHECK(!cache.contains("a"));
}

CACHEX_TEST(a_get_on_an_expired_key_does_not_reorder_the_survivors) {
  cachex::Cache cache;
  cache.set("a", "1");
  cache.set("doomed", "x", kShortTtl);
  cache.set("b", "2");
  std::this_thread::sleep_for(kWaitPastShort);

  cache.get("doomed");  // a miss, and a reclaim

  const std::vector<std::string> order = cache.keys_by_recency();
  CHECK_EQ(order.size(), 2u);
  CHECK_EQ(order.front(), "b");
  CHECK_EQ(order.back(), "a");
}

// The headline cost of lazy expiration: an expired entry still occupies
// capacity until something touches it, so it can push out a live key.
CACHEX_TEST(expired_entries_still_occupy_capacity_until_reclaimed) {
  cachex::Cache cache(2);
  cache.set("doomed", "x", kShortTtl);
  cache.set("live", "y");
  std::this_thread::sleep_for(kWaitPastShort);

  // "doomed" is expired but still resident, so the cache is still full.
  CHECK_EQ(cache.size(), 2u);

  cache.set("newcomer", "z");

  // LRU evicts the oldest entry, which happens to be the expired one here --
  // but it evicted it as the LRU victim, not because it was expired.
  CHECK_EQ(cache.evictions(), 1u);
  CHECK(cache.contains("live"));
  CHECK(cache.contains("newcomer"));
}

CACHEX_TEST(mixed_ttl_and_persistent_keys_coexist) {
  cachex::Cache cache;
  cache.set("forever", "1");
  cache.set("fleeting", "2", kShortTtl);
  cache.set("later", "3", kLongTtl);

  CHECK_EQ(cache.size(), 3u);
  std::this_thread::sleep_for(kWaitPastShort);

  CHECK_EQ(cache.get("forever").value_or(""), "1");
  CHECK(!cache.get("fleeting").has_value());
  CHECK_EQ(cache.get("later").value_or(""), "3");

  CHECK_EQ(cache.size(), 2u);
  CHECK_EQ(cache.expired_removals(), 1u);
}

CACHEX_TEST(expired_removals_counts_each_entry_once) {
  cachex::Cache cache;
  for (int i = 0; i < 10; ++i) {
    cache.set("k" + std::to_string(i), "v", kShortTtl);
  }
  std::this_thread::sleep_for(kWaitPastShort);

  for (int pass = 0; pass < 3; ++pass) {
    for (int i = 0; i < 10; ++i) {
      cache.get("k" + std::to_string(i));  // later passes find nothing
    }
  }

  CHECK_EQ(cache.expired_removals(), 10u);
  CHECK_EQ(cache.size(), 0u);
}

CACHEX_TEST(clear_keeps_the_expired_removal_count) {
  cachex::Cache cache;
  cache.set("k", "v", kShortTtl);
  std::this_thread::sleep_for(kWaitPastShort);
  cache.get("k");
  CHECK_EQ(cache.expired_removals(), 1u);

  cache.clear();
  CHECK_EQ(cache.expired_removals(), 1u);  // a lifetime statistic
}

CACHEX_TEST(a_coarser_duration_converts_implicitly) {
  cachex::Cache cache;
  cache.set("k", "v", std::chrono::seconds(30));

  const cachex::TtlInfo info = cache.ttl("k");
  CHECK_EQ(state_name(info.state), "Expiring");
  CHECK(info.remaining > 29000ms);
  CHECK(info.remaining <= 30000ms);
}
