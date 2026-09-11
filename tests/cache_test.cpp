#include "cachex/cache.hpp"

#include <string>
#include <thread>
#include <vector>

#include "test_framework.hpp"

namespace {

// CHECK_EQ needs operator<<, which std::vector does not have. Comparing the
// joined form also makes a failure readable: "b,a,c" vs "a,b,c".
std::string join(const std::vector<std::string>& items) {
  std::string out;
  for (const std::string& item : items) {
    if (!out.empty()) {
      out += ",";
    }
    out += item;
  }
  return out;
}

}  // namespace

// --- basics ----------------------------------------------------------------

CACHEX_TEST(new_cache_is_empty) {
  const cachex::Cache cache;
  CHECK_EQ(cache.size(), 0u);
  CHECK(cache.empty());
}

CACHEX_TEST(set_then_get_returns_value) {
  cachex::Cache cache;
  cache.set("name", "cachex");

  const auto value = cache.get("name");
  CHECK(value.has_value());
  CHECK_EQ(value.value_or(""), "cachex");
  CHECK_EQ(cache.size(), 1u);
  CHECK(!cache.empty());
}

CACHEX_TEST(get_missing_key_returns_nullopt) {
  cachex::Cache cache;
  cache.set("present", "1");

  CHECK(!cache.get("absent").has_value());
  // A miss must not insert anything -- a classic bug when operator[] is used.
  CHECK_EQ(cache.size(), 1u);
}

CACHEX_TEST(get_on_empty_cache_returns_nullopt) {
  cachex::Cache cache;
  CHECK(!cache.get("anything").has_value());
  CHECK_EQ(cache.size(), 0u);
}

// --- updates ---------------------------------------------------------------

CACHEX_TEST(set_existing_key_overwrites_value) {
  cachex::Cache cache;
  cache.set("k", "first");
  cache.set("k", "second");

  CHECK_EQ(cache.get("k").value_or(""), "second");
}

CACHEX_TEST(set_existing_key_does_not_grow_the_cache) {
  cachex::Cache cache;
  cache.set("k", "a");
  cache.set("k", "b");
  cache.set("k", "c");

  CHECK_EQ(cache.size(), 1u);
}

CACHEX_TEST(repeated_identical_sets_are_idempotent) {
  cachex::Cache cache;
  for (int i = 0; i < 100; ++i) {
    cache.set("k", "v");
  }
  CHECK_EQ(cache.size(), 1u);
  CHECK_EQ(cache.get("k").value_or(""), "v");
}

// --- erase -----------------------------------------------------------------

CACHEX_TEST(erase_removes_the_key_and_reports_true) {
  cachex::Cache cache;
  cache.set("k", "v");

  CHECK(cache.erase("k"));
  CHECK(!cache.contains("k"));
  CHECK(!cache.get("k").has_value());
  CHECK_EQ(cache.size(), 0u);
}

CACHEX_TEST(erase_missing_key_reports_false_and_changes_nothing) {
  cachex::Cache cache;
  cache.set("k", "v");

  CHECK(!cache.erase("absent"));
  CHECK_EQ(cache.size(), 1u);
  CHECK(cache.contains("k"));
}

CACHEX_TEST(erase_twice_reports_false_the_second_time) {
  cachex::Cache cache;
  cache.set("k", "v");

  CHECK(cache.erase("k"));
  CHECK(!cache.erase("k"));
  CHECK_EQ(cache.size(), 0u);
}

CACHEX_TEST(key_can_be_reinserted_after_erase) {
  cachex::Cache cache;
  cache.set("k", "old");
  cache.erase("k");
  cache.set("k", "new");

  CHECK_EQ(cache.get("k").value_or(""), "new");
  CHECK_EQ(cache.size(), 1u);
}

CACHEX_TEST(erasing_one_key_leaves_the_others_intact) {
  cachex::Cache cache;
  cache.set("a", "1");
  cache.set("b", "2");
  cache.set("c", "3");

  CHECK(cache.erase("b"));
  CHECK_EQ(cache.size(), 2u);
  CHECK_EQ(cache.get("a").value_or(""), "1");
  CHECK_EQ(cache.get("c").value_or(""), "3");
  CHECK(!cache.contains("b"));
}

// --- contains --------------------------------------------------------------

CACHEX_TEST(contains_distinguishes_present_from_absent) {
  cachex::Cache cache;
  cache.set("here", "1");

  CHECK(cache.contains("here"));
  CHECK(!cache.contains("not_here"));
}

CACHEX_TEST(contains_does_not_insert_on_a_miss) {
  cachex::Cache cache;
  CHECK(!cache.contains("ghost"));
  CHECK_EQ(cache.size(), 0u);
}

// --- multiple entries ------------------------------------------------------

CACHEX_TEST(many_entries_are_all_retrievable) {
  cachex::Cache cache;
  constexpr int kCount = 1000;

  for (int i = 0; i < kCount; ++i) {
    cache.set("key" + std::to_string(i), "value" + std::to_string(i));
  }
  CHECK_EQ(cache.size(), static_cast<std::size_t>(kCount));

  bool all_correct = true;
  for (int i = 0; i < kCount; ++i) {
    const auto value = cache.get("key" + std::to_string(i));
    if (value.value_or("") != "value" + std::to_string(i)) {
      all_correct = false;
    }
  }
  CHECK(all_correct);
}

CACHEX_TEST(size_tracks_a_mixed_sequence_of_operations) {
  cachex::Cache cache;
  cache.set("a", "1");
  CHECK_EQ(cache.size(), 1u);
  cache.set("b", "2");
  CHECK_EQ(cache.size(), 2u);
  cache.set("a", "overwritten");  // update, not insert
  CHECK_EQ(cache.size(), 2u);
  cache.erase("b");
  CHECK_EQ(cache.size(), 1u);
  cache.erase("missing");
  CHECK_EQ(cache.size(), 1u);
  cache.set("c", "3");
  CHECK_EQ(cache.size(), 2u);
}

CACHEX_TEST(clear_empties_the_cache_and_it_stays_usable) {
  cachex::Cache cache;
  cache.set("a", "1");
  cache.set("b", "2");

  cache.clear();
  CHECK_EQ(cache.size(), 0u);
  CHECK(cache.empty());
  CHECK(!cache.contains("a"));

  cache.set("c", "3");
  CHECK_EQ(cache.get("c").value_or(""), "3");
  CHECK_EQ(cache.size(), 1u);
}

// --- edge cases ------------------------------------------------------------

CACHEX_TEST(empty_key_is_a_valid_key) {
  cachex::Cache cache;
  cache.set("", "empty-key-value");

  CHECK(cache.contains(""));
  CHECK_EQ(cache.get("").value_or("MISSING"), "empty-key-value");
  CHECK_EQ(cache.size(), 1u);
}

CACHEX_TEST(empty_value_is_distinct_from_a_missing_key) {
  cachex::Cache cache;
  cache.set("k", "");

  const auto value = cache.get("k");
  // The whole reason get returns optional rather than a plain string: an empty
  // stored value and an absent key must not look the same to the caller.
  CHECK(value.has_value());
  CHECK_EQ(value.value_or("MISSING"), "");
  CHECK(cache.contains("k"));
}

CACHEX_TEST(empty_key_and_empty_value_together) {
  cachex::Cache cache;
  cache.set("", "");

  CHECK(cache.get("").has_value());
  CHECK_EQ(cache.size(), 1u);
}

CACHEX_TEST(keys_and_values_may_contain_embedded_nulls) {
  cachex::Cache cache;
  const std::string key("a\0b", 3);
  const std::string value("x\0y\0z", 5);
  cache.set(key, value);

  // std::string is length-delimited, not null-terminated, so binary data works.
  // This matters for the network protocol later.
  const auto stored = cache.get(key);
  CHECK(stored.has_value());
  CHECK_EQ(stored.value_or("").size(), 5u);
  CHECK(stored.value_or("") == value);
  CHECK(!cache.contains("a"));  // must not be confused with the truncated key
}

CACHEX_TEST(large_keys_and_values_round_trip) {
  cachex::Cache cache;
  const std::string big_key(10000, 'k');
  const std::string big_value(1000000, 'v');
  cache.set(big_key, big_value);

  const auto stored = cache.get(big_key);
  CHECK(stored.has_value());
  CHECK_EQ(stored.value_or("").size(), 1000000u);
}

CACHEX_TEST(keys_differing_only_in_case_are_distinct) {
  cachex::Cache cache;
  cache.set("Key", "upper");
  cache.set("key", "lower");

  CHECK_EQ(cache.size(), 2u);
  CHECK_EQ(cache.get("Key").value_or(""), "upper");
  CHECK_EQ(cache.get("key").value_or(""), "lower");
}

CACHEX_TEST(returned_value_is_a_copy_not_an_alias) {
  cachex::Cache cache;
  cache.set("k", "original");

  auto value = cache.get("k");
  CHECK(value.has_value());
  if (value.has_value()) {
    value->append("-mutated");
  }

  // Mutating what get() returned must not reach into the cache.
  CHECK_EQ(cache.get("k").value_or(""), "original");
}

CACHEX_TEST(set_accepts_temporaries_and_moved_from_arguments) {
  cachex::Cache cache;
  std::string key = "moved-key";
  std::string value = "moved-value";
  cache.set(std::move(key), std::move(value));

  CHECK_EQ(cache.get("moved-key").value_or(""), "moved-value");
  CHECK_EQ(cache.size(), 1u);
}

// --- recency ordering ------------------------------------------------------
//
// Nothing is evicted yet, so ordering is not externally observable through
// get/set. These tests pin the invariant now so that Stage 3 eviction starts
// from a known-correct order.

CACHEX_TEST(newest_insert_goes_to_the_front) {
  cachex::Cache cache;
  cache.set("a", "1");
  cache.set("b", "2");
  cache.set("c", "3");

  CHECK_EQ(join(cache.keys_by_recency()), "c,b,a");
}

CACHEX_TEST(get_moves_the_key_to_the_front) {
  cachex::Cache cache;
  cache.set("a", "1");
  cache.set("b", "2");
  cache.set("c", "3");

  cache.get("a");
  CHECK_EQ(join(cache.keys_by_recency()), "a,c,b");
}

CACHEX_TEST(get_on_the_newest_key_keeps_the_order) {
  cachex::Cache cache;
  cache.set("a", "1");
  cache.set("b", "2");

  cache.get("b");  // already at the front: splice must be a no-op, not a crash
  CHECK_EQ(join(cache.keys_by_recency()), "b,a");
}

CACHEX_TEST(get_miss_does_not_reorder) {
  cachex::Cache cache;
  cache.set("a", "1");
  cache.set("b", "2");

  cache.get("absent");
  CHECK_EQ(join(cache.keys_by_recency()), "b,a");
}

CACHEX_TEST(updating_an_existing_key_moves_it_to_the_front) {
  cachex::Cache cache;
  cache.set("a", "1");
  cache.set("b", "2");
  cache.set("c", "3");

  cache.set("a", "updated");
  CHECK_EQ(join(cache.keys_by_recency()), "a,c,b");
  CHECK_EQ(cache.get("a").value_or(""), "updated");
}

CACHEX_TEST(contains_deliberately_does_not_reorder) {
  cachex::Cache cache;
  cache.set("a", "1");
  cache.set("b", "2");

  cache.contains("a");
  // Peeking is not using: EXISTS must not rescue a key from eviction.
  CHECK_EQ(join(cache.keys_by_recency()), "b,a");
}

CACHEX_TEST(erase_removes_the_key_from_the_recency_order) {
  cachex::Cache cache;
  cache.set("a", "1");
  cache.set("b", "2");
  cache.set("c", "3");

  cache.erase("b");
  CHECK_EQ(join(cache.keys_by_recency()), "c,a");
}

CACHEX_TEST(recency_order_survives_heavy_churn) {
  cachex::Cache cache;
  for (int i = 0; i < 200; ++i) {
    cache.set("k" + std::to_string(i), "v");
  }
  for (int i = 0; i < 200; i += 2) {
    cache.erase("k" + std::to_string(i));
  }
  for (int i = 1; i < 200; i += 2) {
    cache.get("k" + std::to_string(i));
  }

  const std::vector<std::string> order = cache.keys_by_recency();
  // The list and the map must agree on how many entries exist -- a mismatch is
  // exactly what a stale iterator or a half-finished erase would produce.
  CHECK_EQ(order.size(), cache.size());
  CHECK_EQ(order.size(), 100u);
  CHECK_EQ(order.front(), "k199");  // the last key touched
}

// --- get_into --------------------------------------------------------------

CACHEX_TEST(get_into_returns_the_value_through_the_caller_buffer) {
  cachex::Cache cache;
  cache.set("k", "value");

  std::string out;
  CHECK(cache.get_into("k", out));
  CHECK_EQ(out, "value");
}

CACHEX_TEST(get_into_reports_a_miss_without_touching_the_buffer) {
  cachex::Cache cache;
  std::string out = "untouched";

  CHECK(!cache.get_into("absent", out));
  CHECK_EQ(out, "untouched");
  CHECK_EQ(cache.size(), 0u);  // a miss must not insert
}

CACHEX_TEST(get_into_agrees_with_get_on_every_case) {
  cachex::Cache cache;
  cache.set("present", "value");
  cache.set("empty", "");

  std::string out;
  CHECK_EQ(cache.get_into("present", out), cache.get("present").has_value());
  CHECK_EQ(out, cache.get("present").value_or("?"));

  CHECK(cache.get_into("empty", out));
  CHECK_EQ(out, "");  // an empty value is a hit, not a miss

  CHECK_EQ(cache.get_into("absent", out), cache.get("absent").has_value());
}

CACHEX_TEST(get_into_counts_as_a_use_like_get_does) {
  cachex::Cache cache(3);
  cache.set("a", "1");
  cache.set("b", "2");
  cache.set("c", "3");

  std::string out;
  cache.get_into("a", out);   // must promote "a" exactly as get() would
  cache.set("d", "4");

  CHECK(cache.contains("a"));
  CHECK(!cache.contains("b"));
}

CACHEX_TEST(get_into_reuses_the_buffer_across_calls) {
  cachex::Cache cache;
  cache.set("long", std::string(500, 'x'));
  cache.set("short", "s");

  std::string out;
  CHECK(cache.get_into("long", out));
  const std::size_t capacity_after_long = out.capacity();

  // Reading a short value must not shrink the buffer -- that reuse is the whole
  // reason this overload exists.
  CHECK(cache.get_into("short", out));
  CHECK_EQ(out, "s");
  CHECK(out.capacity() >= capacity_after_long);
}

CACHEX_TEST(get_into_reclaims_an_expired_entry_like_get_does) {
  cachex::Cache cache;
  cache.set("k", "v", std::chrono::milliseconds(30));
  std::this_thread::sleep_for(std::chrono::milliseconds(130));

  std::string out = "untouched";
  CHECK(!cache.get_into("k", out));
  CHECK_EQ(out, "untouched");
  CHECK_EQ(cache.size(), 0u);
  CHECK_EQ(cache.expired_removals(), 1u);
}
