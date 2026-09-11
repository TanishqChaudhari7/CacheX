#include "cachex/cache.hpp"

#include <string>
#include <vector>

#include "test_framework.hpp"

namespace {

// Renders the recency order as "mru,...,lru" so a failure shows the whole state
// rather than just which key went missing.
std::string order(const cachex::Cache& cache) {
  std::string out;
  for (const std::string& key : cache.keys_by_recency()) {
    if (!out.empty()) {
      out += ",";
    }
    out += key;
  }
  return out;
}

// Fills a cache with single-character keys in the order given, so each test can
// state its starting recency order in one line.
void fill(cachex::Cache& cache, const std::string& keys) {
  for (const char key : keys) {
    cache.set(std::string(1, key), "v");
  }
}

}  // namespace

// --- capacity plumbing -----------------------------------------------------

CACHEX_TEST(default_constructed_cache_is_unbounded) {
  const cachex::Cache cache;
  CHECK(!cache.capacity().has_value());
}

CACHEX_TEST(capacity_is_reported) {
  const cachex::Cache cache(10);
  CHECK(cache.capacity().has_value());
  CHECK_EQ(cache.capacity().value_or(0), 10u);
}

CACHEX_TEST(unbounded_cache_never_evicts) {
  cachex::Cache cache;
  for (int i = 0; i < 5000; ++i) {
    cache.set("k" + std::to_string(i), "v");
  }
  CHECK_EQ(cache.size(), 5000u);
  CHECK_EQ(cache.evictions(), 0u);
}

// --- eviction at capacity --------------------------------------------------

CACHEX_TEST(inserting_past_capacity_evicts_the_least_recently_used) {
  cachex::Cache cache(3);
  fill(cache, "abc");
  CHECK_EQ(order(cache), "c,b,a");  // "a" is the oldest, so it is next to go

  cache.set("d", "v");

  CHECK_EQ(order(cache), "d,c,b");
  CHECK_EQ(cache.size(), 3u);
  CHECK(!cache.contains("a"));
  CHECK(cache.contains("b"));
  CHECK(cache.contains("c"));
  CHECK(cache.contains("d"));
}

CACHEX_TEST(size_never_exceeds_capacity) {
  cachex::Cache cache(50);
  for (int i = 0; i < 1000; ++i) {
    cache.set("k" + std::to_string(i), "v");
    CHECK(cache.size() <= 50u);
  }
  CHECK_EQ(cache.size(), 50u);
}

CACHEX_TEST(only_the_most_recent_capacity_keys_survive_a_linear_scan) {
  cachex::Cache cache(10);
  for (int i = 0; i < 100; ++i) {
    cache.set("k" + std::to_string(i), "v" + std::to_string(i));
  }

  // Keys 0..89 should be gone, 90..99 should remain, and the values must have
  // travelled with the keys.
  bool old_keys_all_gone = true;
  for (int i = 0; i < 90; ++i) {
    if (cache.contains("k" + std::to_string(i))) {
      old_keys_all_gone = false;
    }
  }
  bool recent_keys_all_present = true;
  for (int i = 90; i < 100; ++i) {
    if (cache.get("k" + std::to_string(i)).value_or("") != "v" + std::to_string(i)) {
      recent_keys_all_present = false;
    }
  }
  CHECK(old_keys_all_gone);
  CHECK(recent_keys_all_present);
  CHECK_EQ(cache.size(), 10u);
}

CACHEX_TEST(eviction_counter_counts_only_evictions) {
  cachex::Cache cache(3);
  fill(cache, "abc");
  CHECK_EQ(cache.evictions(), 0u);  // filling to capacity evicts nothing

  cache.set("d", "v");
  cache.set("e", "v");
  CHECK_EQ(cache.evictions(), 2u);

  cache.set("e", "updated");  // an update is not an insert
  CHECK_EQ(cache.evictions(), 2u);

  cache.erase("e");           // an explicit erase is not an eviction
  CHECK_EQ(cache.evictions(), 2u);
}

// --- GET updates recency ---------------------------------------------------

CACHEX_TEST(a_recently_read_key_survives_eviction) {
  cachex::Cache cache(3);
  fill(cache, "abc");         // order: c,b,a -- "a" would be evicted next

  cache.get("a");             // reading "a" makes it the newest
  CHECK_EQ(order(cache), "a,c,b");

  cache.set("d", "v");        // so "b" is now the oldest and goes instead

  CHECK_EQ(order(cache), "d,a,c");
  CHECK(cache.contains("a"));
  CHECK(!cache.contains("b"));
}

CACHEX_TEST(repeated_gets_keep_moving_the_key_to_the_front) {
  cachex::Cache cache(3);
  fill(cache, "abc");         // c,b,a

  cache.get("a");             // a,c,b
  CHECK_EQ(order(cache), "a,c,b");
  cache.get("b");             // b,a,c
  CHECK_EQ(order(cache), "b,a,c");
  cache.get("a");             // a,b,c
  CHECK_EQ(order(cache), "a,b,c");

  // "c" has now been untouched the longest even though it was inserted last.
  cache.set("d", "v");
  CHECK_EQ(order(cache), "d,a,b");
  CHECK(!cache.contains("c"));
}

CACHEX_TEST(reading_the_same_key_repeatedly_pins_it) {
  cachex::Cache cache(3);
  fill(cache, "abc");

  // A key that is read before every insertion should never be evicted.
  for (int i = 0; i < 50; ++i) {
    cache.get("a");
    cache.set("new" + std::to_string(i), "v");
  }
  CHECK(cache.contains("a"));
  CHECK_EQ(cache.size(), 3u);
}

CACHEX_TEST(a_get_miss_does_not_disturb_the_eviction_order) {
  cachex::Cache cache(3);
  fill(cache, "abc");

  cache.get("nonexistent");
  CHECK_EQ(order(cache), "c,b,a");

  cache.set("d", "v");
  CHECK(!cache.contains("a"));  // still the original victim
}

CACHEX_TEST(contains_does_not_rescue_a_key_from_eviction) {
  cachex::Cache cache(3);
  fill(cache, "abc");

  cache.contains("a");  // peeking is deliberately not using
  cache.set("d", "v");

  CHECK(!cache.contains("a"));
  CHECK_EQ(order(cache), "d,c,b");
}

// --- SET updates recency ---------------------------------------------------

CACHEX_TEST(updating_an_existing_key_makes_it_most_recently_used) {
  cachex::Cache cache(3);
  fill(cache, "abc");         // c,b,a

  cache.set("a", "updated");  // a write counts as a use
  CHECK_EQ(order(cache), "a,c,b");

  cache.set("d", "v");        // so "b" is evicted, not "a"
  CHECK(cache.contains("a"));
  CHECK(!cache.contains("b"));
  CHECK_EQ(cache.get("a").value_or(""), "updated");
}

CACHEX_TEST(updating_at_capacity_evicts_nothing) {
  cachex::Cache cache(3);
  fill(cache, "abc");

  cache.set("b", "updated");
  CHECK_EQ(cache.size(), 3u);
  CHECK_EQ(cache.evictions(), 0u);
  CHECK(cache.contains("a"));
}

CACHEX_TEST(re_inserting_an_evicted_key_treats_it_as_new) {
  cachex::Cache cache(2);
  fill(cache, "ab");
  cache.set("c", "v");        // evicts "a"
  CHECK(!cache.contains("a"));

  cache.set("a", "back");     // a fresh insert, which evicts "b"
  CHECK_EQ(order(cache), "a,c");
  CHECK_EQ(cache.get("a").value_or(""), "back");
  CHECK(!cache.contains("b"));
}

// --- erase interacts with capacity -----------------------------------------

CACHEX_TEST(erase_frees_a_slot_so_the_next_insert_does_not_evict) {
  cachex::Cache cache(3);
  fill(cache, "abc");

  cache.erase("b");
  cache.set("d", "v");

  CHECK_EQ(cache.evictions(), 0u);
  CHECK_EQ(order(cache), "d,c,a");
  CHECK(cache.contains("a"));
}

// --- boundary capacities ---------------------------------------------------

CACHEX_TEST(capacity_one_holds_only_the_newest_key) {
  cachex::Cache cache(1);

  cache.set("a", "1");
  CHECK_EQ(cache.size(), 1u);
  CHECK_EQ(cache.get("a").value_or(""), "1");

  cache.set("b", "2");
  CHECK_EQ(cache.size(), 1u);
  CHECK(!cache.contains("a"));
  CHECK_EQ(cache.get("b").value_or(""), "2");
  CHECK_EQ(cache.evictions(), 1u);
}

CACHEX_TEST(capacity_one_handles_updates_without_evicting) {
  cachex::Cache cache(1);
  cache.set("a", "1");
  cache.set("a", "2");

  CHECK_EQ(cache.size(), 1u);
  CHECK_EQ(cache.evictions(), 0u);
  CHECK_EQ(cache.get("a").value_or(""), "2");
}

CACHEX_TEST(capacity_zero_stores_nothing) {
  cachex::Cache cache(0);

  cache.set("a", "1");
  CHECK_EQ(cache.size(), 0u);
  CHECK(cache.empty());
  CHECK(!cache.contains("a"));
  CHECK(!cache.get("a").has_value());
  CHECK_EQ(cache.evictions(), 1u);
}

CACHEX_TEST(capacity_larger_than_the_entry_count_never_evicts) {
  cachex::Cache cache(100);
  for (int i = 0; i < 10; ++i) {
    cache.set("k" + std::to_string(i), "v");
  }

  CHECK_EQ(cache.size(), 10u);
  CHECK_EQ(cache.evictions(), 0u);
  bool all_present = true;
  for (int i = 0; i < 10; ++i) {
    if (!cache.contains("k" + std::to_string(i))) {
      all_present = false;
    }
  }
  CHECK(all_present);
}

CACHEX_TEST(filling_exactly_to_capacity_evicts_nothing) {
  cachex::Cache cache(5);
  for (int i = 0; i < 5; ++i) {
    cache.set("k" + std::to_string(i), "v");
  }

  CHECK_EQ(cache.size(), 5u);
  CHECK_EQ(cache.evictions(), 0u);

  cache.set("one-too-many", "v");  // the very next insert does evict
  CHECK_EQ(cache.evictions(), 1u);
  CHECK_EQ(cache.size(), 5u);
}

// --- other interactions ----------------------------------------------------

CACHEX_TEST(clear_keeps_capacity_and_the_lifetime_eviction_count) {
  cachex::Cache cache(2);
  fill(cache, "abc");                // one eviction
  CHECK_EQ(cache.evictions(), 1u);

  cache.clear();
  CHECK_EQ(cache.size(), 0u);
  CHECK_EQ(cache.capacity().value_or(0), 2u);
  CHECK_EQ(cache.evictions(), 1u);   // a lifetime statistic, not a live one

  fill(cache, "xyz");                // capacity still applies after clear()
  CHECK_EQ(cache.size(), 2u);
  CHECK_EQ(order(cache), "z,y");
}

CACHEX_TEST(empty_key_participates_in_eviction_like_any_other) {
  cachex::Cache cache(2);
  cache.set("", "empty-key");
  cache.set("a", "1");
  cache.set("b", "2");  // evicts the empty key, which was oldest

  CHECK(!cache.contains(""));
  CHECK_EQ(order(cache), "b,a");
}

// The invariant that matters most: under churn plus reads, the two structures
// must stay exactly in step. A leaked list node or a stale map entry shows up
// here as a size mismatch.
CACHEX_TEST(map_and_list_stay_in_step_under_mixed_churn) {
  cachex::Cache cache(64);

  for (int i = 0; i < 20000; ++i) {
    cache.set("k" + std::to_string(i % 500), "v");
    if (i % 3 == 0) {
      cache.get("k" + std::to_string(i % 97));
    }
    if (i % 7 == 0) {
      cache.erase("k" + std::to_string(i % 31));
    }
  }

  const std::vector<std::string> recency = cache.keys_by_recency();
  CHECK_EQ(recency.size(), cache.size());
  CHECK(cache.size() <= 64u);

  // Every key in the recency list must also be findable through the index.
  bool every_listed_key_is_indexed = true;
  for (const std::string& key : recency) {
    if (!cache.contains(key)) {
      every_listed_key_is_indexed = false;
    }
  }
  CHECK(every_listed_key_is_indexed);
}
