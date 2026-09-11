#include "cachex/sharded_cache.hpp"

#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "test_framework.hpp"

namespace {

template <typename F>
void run_parallel(int count, F worker) {
  std::vector<std::thread> threads;
  threads.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    threads.emplace_back([&worker, i] { worker(i); });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
}

/// Sums one shard's recency list, for invariant checks.
std::size_t resident_in_shard(const cachex::ShardedCache& cache,
                              std::size_t index) {
  return cache.shard_at(index).keys_by_recency().size();
}

}  // namespace

// --- shard selection -------------------------------------------------------

CACHEX_TEST(shard_selection_is_deterministic) {
  const cachex::ShardedCache cache(8);
  const std::size_t first = cache.shard_index_for("some-key");
  for (int i = 0; i < 100; ++i) {
    CHECK_EQ(cache.shard_index_for("some-key"), first);
  }
}

CACHEX_TEST(shard_index_is_always_in_range) {
  for (const std::size_t shards : {1u, 2u, 3u, 4u, 8u, 16u, 64u}) {
    const cachex::ShardedCache cache(shards);
    CHECK_EQ(cache.shard_count(), shards);
    bool in_range = true;
    for (int i = 0; i < 2000; ++i) {
      if (cache.shard_index_for("key" + std::to_string(i)) >= shards) {
        in_range = false;
      }
    }
    CHECK(in_range);
  }
}

// A hash that clustered keys into a few shards would defeat the whole point:
// the crowded shards would contend exactly as badly as one global mutex.
CACHEX_TEST(keys_spread_reasonably_evenly_across_shards) {
  const cachex::ShardedCache cache(8);
  std::vector<int> counts(8, 0);
  constexpr int kKeys = 8000;
  for (int i = 0; i < kKeys; ++i) {
    ++counts[cache.shard_index_for("key:" + std::to_string(i))];
  }

  // Perfectly even would be 1000 per shard. Allow a generous +/-25% -- the test
  // is looking for gross clustering, not proving uniformity.
  bool balanced = true;
  for (const int count : counts) {
    if (count < 750 || count > 1250) {
      balanced = false;
    }
  }
  CHECK(balanced);
}

CACHEX_TEST(a_single_shard_receives_every_key) {
  const cachex::ShardedCache cache(1);
  CHECK_EQ(cache.shard_count(), 1u);
  bool all_zero = true;
  for (int i = 0; i < 500; ++i) {
    if (cache.shard_index_for("k" + std::to_string(i)) != 0) {
      all_zero = false;
    }
  }
  CHECK(all_zero);
}

CACHEX_TEST(a_key_lands_in_the_shard_that_actually_stores_it) {
  cachex::ShardedCache cache(4);
  cache.set("findme", "value");

  const std::size_t expected = cache.shard_index_for("findme");
  CHECK(cache.shard_at(expected).contains("findme"));

  // ...and in none of the others.
  std::size_t elsewhere = 0;
  for (std::size_t i = 0; i < cache.shard_count(); ++i) {
    if (i != expected && cache.shard_at(i).contains("findme")) {
      ++elsewhere;
    }
  }
  CHECK_EQ(elsewhere, 0u);
}

CACHEX_TEST(shard_hash_differs_from_raw_std_hash) {
  // If shard selection used std::hash directly, it would correlate with the
  // bucket index the shard's own unordered_map computes from the same value.
  int differences = 0;
  for (int i = 0; i < 100; ++i) {
    const std::string key = "key" + std::to_string(i);
    if (cachex::shard_hash(key) !=
        static_cast<std::uint64_t>(std::hash<std::string>{}(key))) {
      ++differences;
    }
  }
  CHECK_EQ(differences, 100);
}

// --- the API is unchanged --------------------------------------------------

CACHEX_TEST(sharded_cache_behaves_like_the_unsharded_one) {
  cachex::ShardedCache cache(8);

  cache.set("a", "1");
  cache.set("b", "2");
  CHECK_EQ(cache.get("a").value_or(""), "1");
  CHECK_EQ(cache.get("b").value_or(""), "2");
  CHECK(!cache.get("missing").has_value());
  CHECK(cache.contains("a"));
  CHECK_EQ(cache.size(), 2u);

  CHECK(cache.erase("a"));
  CHECK(!cache.erase("a"));
  CHECK_EQ(cache.size(), 1u);

  cache.set("c", "3", std::chrono::seconds(60));
  CHECK_EQ(cache.ttl("c").state == cachex::TtlState::Expiring, true);
  CHECK_EQ(cache.ttl("b").state == cachex::TtlState::Persistent, true);
  CHECK_EQ(cache.ttl("nope").state == cachex::TtlState::Missing, true);

  cache.clear();
  CHECK_EQ(cache.size(), 0u);
  CHECK(cache.empty());
}

CACHEX_TEST(many_keys_all_round_trip_across_shards) {
  cachex::ShardedCache cache(8);
  constexpr int kKeys = 5000;
  for (int i = 0; i < kKeys; ++i) {
    cache.set("k" + std::to_string(i), "v" + std::to_string(i));
  }
  CHECK_EQ(cache.size(), static_cast<std::size_t>(kKeys));

  bool all_correct = true;
  for (int i = 0; i < kKeys; ++i) {
    if (cache.get("k" + std::to_string(i)).value_or("") != "v" + std::to_string(i)) {
      all_correct = false;
    }
  }
  CHECK(all_correct);
}

// --- capacity distribution -------------------------------------------------

CACHEX_TEST(total_capacity_is_split_exactly_across_shards) {
  const cachex::ShardedCache cache(8, 1000);
  CHECK_EQ(cache.capacity().value_or(0), 1000u);

  std::size_t sum = 0;
  for (std::size_t i = 0; i < cache.shard_count(); ++i) {
    sum += cache.shard_at(i).capacity().value_or(0);
  }
  CHECK_EQ(sum, 1000u);
}

// 10 across 4 shards is 2.5 each, which does not exist. The remainder goes to
// the first shards so the parts still sum to exactly 10.
CACHEX_TEST(an_uneven_split_still_sums_to_the_requested_total) {
  const cachex::ShardedCache cache(4, 10);
  CHECK_EQ(cache.capacity().value_or(0), 10u);
  CHECK_EQ(cache.shard_at(0).capacity().value_or(0), 3u);
  CHECK_EQ(cache.shard_at(1).capacity().value_or(0), 3u);
  CHECK_EQ(cache.shard_at(2).capacity().value_or(0), 2u);
  CHECK_EQ(cache.shard_at(3).capacity().value_or(0), 2u);
}

// Otherwise some shards would get capacity 0 and silently swallow every key
// that hashed to them.
CACHEX_TEST(shard_count_is_reduced_rather_than_leaving_a_shard_at_zero) {
  const cachex::ShardedCache cache(16, 4);
  CHECK_EQ(cache.shard_count(), 4u);
  CHECK_EQ(cache.capacity().value_or(0), 4u);

  bool none_zero = true;
  for (std::size_t i = 0; i < cache.shard_count(); ++i) {
    if (cache.shard_at(i).capacity().value_or(0) == 0) {
      none_zero = false;
    }
  }
  CHECK(none_zero);
}

CACHEX_TEST(an_unbounded_sharded_cache_reports_no_capacity) {
  const cachex::ShardedCache cache(4);
  CHECK(!cache.capacity().has_value());
}

CACHEX_TEST(total_size_never_exceeds_total_capacity) {
  cachex::ShardedCache cache(8, 200);
  for (int i = 0; i < 5000; ++i) {
    cache.set("k" + std::to_string(i), "v");
    CHECK(cache.size() <= 200u);
  }
  CHECK(cache.evictions() > 0u);
}

// --- shards are independent ------------------------------------------------

CACHEX_TEST(filling_one_shard_does_not_evict_from_another) {
  cachex::ShardedCache cache(4, 40);  // 10 per shard

  // Find two keys that land in different shards.
  std::string key_a;
  std::string key_b;
  for (int i = 0; i < 200 && (key_a.empty() || key_b.empty()); ++i) {
    const std::string key = "probe" + std::to_string(i);
    if (cache.shard_index_for(key) == 0 && key_a.empty()) {
      key_a = key;
    }
    if (cache.shard_index_for(key) == 1 && key_b.empty()) {
      key_b = key;
    }
  }
  CHECK(!key_a.empty());
  CHECK(!key_b.empty());

  cache.set(key_a, "in shard 0");
  cache.set(key_b, "in shard 1");

  // Flood shard 0 well past its 10-entry share.
  int added = 0;
  for (int i = 0; i < 5000 && added < 50; ++i) {
    const std::string key = "flood" + std::to_string(i);
    if (cache.shard_index_for(key) == 0) {
      cache.set(key, "x");
      ++added;
    }
  }

  // key_a was pushed out of shard 0; key_b, in a different shard, is untouched.
  CHECK(!cache.contains(key_a));
  CHECK_EQ(cache.get(key_b).value_or(""), "in shard 1");
}

CACHEX_TEST(lru_order_is_maintained_within_a_shard) {
  cachex::ShardedCache cache(4, 40);

  // Collect three keys that share one shard, then exercise LRU inside it.
  std::vector<std::string> same_shard;
  for (int i = 0; i < 2000 && same_shard.size() < 3; ++i) {
    const std::string key = "k" + std::to_string(i);
    if (cache.shard_index_for(key) == 2) {
      same_shard.push_back(key);
    }
  }
  CHECK_EQ(same_shard.size(), 3u);

  for (const std::string& key : same_shard) {
    cache.set(key, "v");
  }
  // Reading the oldest promotes it inside its shard's recency list.
  cache.get(same_shard[0]);

  const std::vector<std::string> order = cache.shard_at(2).keys_by_recency();
  CHECK_EQ(order.front(), same_shard[0]);
}

// --- concurrency -----------------------------------------------------------

CACHEX_TEST(concurrent_writers_across_shards_all_survive) {
  cachex::ShardedCache cache(8);
  constexpr int kThreads = 8;
  constexpr int kPerThread = 2000;

  run_parallel(kThreads, [&cache](int thread_id) {
    for (int i = 0; i < kPerThread; ++i) {
      cache.set("t" + std::to_string(thread_id) + ":" + std::to_string(i), "v");
    }
  });

  CHECK_EQ(cache.size(), static_cast<std::size_t>(kThreads * kPerThread));

  // Every shard's map and recency list must still agree with each other.
  std::size_t summed = 0;
  for (std::size_t i = 0; i < cache.shard_count(); ++i) {
    CHECK_EQ(resident_in_shard(cache, i), cache.shard_at(i).size());
    summed += cache.shard_at(i).size();
  }
  CHECK_EQ(summed, cache.size());
}

CACHEX_TEST(concurrent_mixed_operations_keep_every_shard_consistent) {
  cachex::ShardedCache cache(8, 400);
  std::atomic<int> errors{0};

  run_parallel(8, [&cache, &errors](int thread_id) {
    for (int i = 0; i < 3000; ++i) {
      const std::string key = "k" + std::to_string((thread_id * 17 + i) % 1000);
      switch (i % 5) {
        case 0: cache.set(key, "value"); break;
        case 1: cache.get(key); break;
        case 2: cache.erase(key); break;
        case 3: cache.ttl(key); break;
        case 4:
          if (const auto value = cache.get(key)) {
            if (value->empty()) {
              errors.fetch_add(1);
            }
          }
          break;
        default: break;
      }
    }
  });

  CHECK_EQ(errors.load(), 0);
  CHECK(cache.size() <= 400u);
  for (std::size_t i = 0; i < cache.shard_count(); ++i) {
    CHECK_EQ(resident_in_shard(cache, i), cache.shard_at(i).size());
    CHECK(cache.shard_at(i).size() <= 50u);  // 400 / 8
  }
}

// Every thread hammers keys in ONE shard, so this is the worst case for
// sharding: all the contention lands on a single mutex.
CACHEX_TEST(concurrent_access_to_a_single_shard_is_still_correct) {
  cachex::ShardedCache cache(8);

  std::vector<std::string> hot_keys;
  for (int i = 0; i < 5000 && hot_keys.size() < 20; ++i) {
    const std::string key = "h" + std::to_string(i);
    if (cache.shard_index_for(key) == 3) {
      hot_keys.push_back(key);
    }
  }
  CHECK_EQ(hot_keys.size(), 20u);

  run_parallel(8, [&cache, &hot_keys](int) {
    for (int i = 0; i < 2000; ++i) {
      const std::string& key = hot_keys[static_cast<std::size_t>(i) % hot_keys.size()];
      cache.set(key, "v");
      cache.get(key);
    }
  });

  CHECK_EQ(cache.size(), 20u);
  CHECK_EQ(resident_in_shard(cache, 3), 20u);
}

CACHEX_TEST(shard_count_does_not_change_observable_behaviour) {
  // The same operations against 1, 4 and 16 shards must agree on everything the
  // caller can see -- which is what makes shard count a pure tuning knob.
  for (const std::size_t shards : {1u, 4u, 16u}) {
    cachex::ShardedCache cache(shards);
    cache.set("x", "1");
    cache.set("y", "2");
    cache.set("x", "updated");

    CHECK_EQ(cache.get("x").value_or(""), "updated");
    CHECK_EQ(cache.get("y").value_or(""), "2");
    CHECK_EQ(cache.size(), 2u);
    CHECK(cache.erase("y"));
    CHECK_EQ(cache.size(), 1u);
    CHECK(!cache.contains("y"));
  }
}
