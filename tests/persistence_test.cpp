#include "cachex/persistence.hpp"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "cachex/sharded_cache.hpp"
#include "test_framework.hpp"

namespace {

using namespace std::chrono_literals;

/// A snapshot path that cleans itself up, unique per test so the tests never
/// collide with each other or with a leftover file from a previous run.
class TempSnapshot {
 public:
  explicit TempSnapshot(const std::string& label) {
    static std::atomic<int> counter{0};
    path_ = "/tmp/cachex_test_" + label + "_" +
            std::to_string(counter.fetch_add(1)) + "_" +
            std::to_string(::getpid()) + ".cxs";
    remove();
  }
  ~TempSnapshot() { remove(); }

  TempSnapshot(const TempSnapshot&) = delete;
  TempSnapshot& operator=(const TempSnapshot&) = delete;

  const std::string& path() const { return path_; }

  void write_raw(const std::string& contents) const {
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    out << contents;
  }

  std::string read_raw() const {
    std::ifstream in(path_, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
  }

  bool file_exists() const {
    std::ifstream in(path_, std::ios::binary);
    return in.good();
  }

 private:
  void remove() const {
    std::remove(path_.c_str());
    std::remove((path_ + ".tmp").c_str());
  }
  std::string path_;
};

}  // namespace

// --- saving ----------------------------------------------------------------

CACHEX_TEST(saving_an_empty_cache_writes_a_valid_empty_snapshot) {
  const TempSnapshot temp("empty");
  cachex::ShardedCache cache(4);
  const cachex::PersistenceManager manager(temp.path());

  const auto saved = manager.save(cache);
  CHECK(saved.ok);
  CHECK_EQ(saved.entries, 0u);
  CHECK(saved.bytes > 0u);  // the header is still written
  CHECK(temp.file_exists());

  // ...and it loads back as an empty cache rather than as an error.
  cachex::ShardedCache restored(4);
  const auto loaded = manager.load(restored);
  CHECK(loaded.ok);
  CHECK_EQ(loaded.loaded, 0u);
  CHECK_EQ(restored.size(), 0u);
}

CACHEX_TEST(saving_a_populated_cache_reports_the_entry_count) {
  const TempSnapshot temp("populated");
  cachex::ShardedCache cache(4);
  for (int i = 0; i < 250; ++i) {
    cache.set("k" + std::to_string(i), "v" + std::to_string(i));
  }

  const cachex::PersistenceManager manager(temp.path());
  const auto saved = manager.save(cache);
  CHECK(saved.ok);
  CHECK_EQ(saved.entries, 250u);
  CHECK(saved.bytes > 250u);
}

CACHEX_TEST(the_snapshot_starts_with_the_magic_and_version) {
  const TempSnapshot temp("magic");
  cachex::ShardedCache cache(1);
  cache.set("k", "v");
  cachex::PersistenceManager(temp.path()).save(cache);

  const std::string contents = temp.read_raw();
  CHECK(contents.rfind("CACHEX-SNAPSHOT 1 ", 0) == 0);
}

// --- loading ---------------------------------------------------------------

CACHEX_TEST(a_populated_cache_round_trips) {
  const TempSnapshot temp("roundtrip");
  cachex::ShardedCache original(4);
  for (int i = 0; i < 500; ++i) {
    original.set("key" + std::to_string(i), "value" + std::to_string(i));
  }
  CHECK(cachex::PersistenceManager(temp.path()).save(original).ok);

  cachex::ShardedCache restored(4);
  const auto loaded = cachex::PersistenceManager(temp.path()).load(restored);
  CHECK(loaded.ok);
  CHECK_EQ(loaded.loaded, 500u);
  CHECK_EQ(restored.size(), 500u);

  bool all_correct = true;
  for (int i = 0; i < 500; ++i) {
    if (restored.get("key" + std::to_string(i)).value_or("") !=
        "value" + std::to_string(i)) {
      all_correct = false;
    }
  }
  CHECK(all_correct);
}

// The wire protocol cannot express these, but the cache can hold them and the
// snapshot is length-prefixed, so it can too.
CACHEX_TEST(keys_and_values_with_awkward_bytes_round_trip) {
  const TempSnapshot temp("binary");
  cachex::ShardedCache original(2);
  original.set("with space", "value with spaces");
  original.set("with\nnewline", "value\nwith\nnewlines");
  original.set(std::string("with\0nul", 8), std::string("val\0ue", 6));
  original.set("", "empty key");
  original.set("empty value", "");
  CHECK(cachex::PersistenceManager(temp.path()).save(original).ok);

  cachex::ShardedCache restored(2);
  CHECK(cachex::PersistenceManager(temp.path()).load(restored).ok);

  CHECK_EQ(restored.get("with space").value_or(""), "value with spaces");
  CHECK_EQ(restored.get("with\nnewline").value_or(""), "value\nwith\nnewlines");
  CHECK(restored.get(std::string("with\0nul", 8)).value_or("") ==
        std::string("val\0ue", 6));
  CHECK_EQ(restored.get("").value_or("MISSING"), "empty key");
  CHECK_EQ(restored.get("empty value").value_or("MISSING"), "");
  CHECK_EQ(restored.size(), 5u);
}

CACHEX_TEST(a_snapshot_loads_into_a_different_shard_count) {
  const TempSnapshot temp("reshard");
  cachex::ShardedCache original(2);
  for (int i = 0; i < 100; ++i) {
    original.set("k" + std::to_string(i), "v");
  }
  CHECK(cachex::PersistenceManager(temp.path()).save(original).ok);

  // The snapshot stores keys, not shard assignments, so the shard count is free
  // to change between runs.
  cachex::ShardedCache restored(16);
  CHECK(cachex::PersistenceManager(temp.path()).load(restored).ok);
  CHECK_EQ(restored.size(), 100u);
  CHECK(restored.contains("k42"));
}

// --- TTL -------------------------------------------------------------------

CACHEX_TEST(a_ttl_survives_a_save_and_load_as_remaining_time) {
  const TempSnapshot temp("ttl");
  cachex::ShardedCache original(2);
  original.set("forever", "1");
  original.set("timed", "2", 60s);
  CHECK(cachex::PersistenceManager(temp.path()).save(original).ok);

  cachex::ShardedCache restored(2);
  CHECK(cachex::PersistenceManager(temp.path()).load(restored).ok);

  CHECK(restored.ttl("forever").state == cachex::TtlState::Persistent);

  const cachex::TtlInfo info = restored.ttl("timed");
  CHECK(info.state == cachex::TtlState::Expiring);
  // Some time passed during the save and load, so it must be a little less
  // than 60 s -- but not much.
  CHECK(info.remaining <= 60s);
  CHECK(info.remaining > 55s);
}

CACHEX_TEST(entries_that_expire_before_the_save_are_not_written) {
  const TempSnapshot temp("expired_save");
  cachex::ShardedCache cache(2);
  cache.set("keep", "1");
  cache.set("doomed", "2", 30ms);
  std::this_thread::sleep_for(150ms);

  const auto saved = cachex::PersistenceManager(temp.path()).save(cache);
  CHECK(saved.ok);
  CHECK_EQ(saved.entries, 1u);  // "doomed" was already invisible

  cachex::ShardedCache restored(2);
  CHECK(cachex::PersistenceManager(temp.path()).load(restored).ok);
  CHECK(restored.contains("keep"));
  CHECK(!restored.contains("doomed"));
}

CACHEX_TEST(entries_that_expire_between_save_and_load_are_skipped) {
  const TempSnapshot temp("expired_load");
  cachex::ShardedCache cache(2);
  cache.set("keep", "1");
  cache.set("fleeting", "2", 60ms);
  CHECK(cachex::PersistenceManager(temp.path()).save(cache).ok);

  // The snapshot holds "fleeting" with ~60 ms left; by the time it is loaded
  // that has run out.
  std::this_thread::sleep_for(200ms);

  cachex::ShardedCache restored(2);
  const auto loaded = cachex::PersistenceManager(temp.path()).load(restored);
  CHECK(loaded.ok);
  CHECK_EQ(loaded.loaded, 1u);
  CHECK_EQ(loaded.expired, 1u);
  CHECK(restored.contains("keep"));
  CHECK(!restored.contains("fleeting"));
}

// --- missing and malformed files -------------------------------------------

// A server starting for the first time has no snapshot. That is normal, not a
// failure, so it must not stop the server from starting.
CACHEX_TEST(a_missing_snapshot_is_not_an_error) {
  const TempSnapshot temp("missing");
  cachex::ShardedCache cache(4);

  const cachex::PersistenceManager manager(temp.path());
  CHECK(!manager.exists());

  const auto loaded = manager.load(cache);
  CHECK(loaded.ok);
  CHECK_EQ(loaded.loaded, 0u);
  CHECK_EQ(cache.size(), 0u);
}

CACHEX_TEST(malformed_snapshots_are_rejected_with_a_reason) {
  const struct {
    const char* label;
    const char* contents;
  } cases[] = {
      {"not a snapshot at all", "hello world\n"},
      {"header missing the timestamp", "CACHEX-SNAPSHOT 1\n0\n"},
      {"wrong magic", "REDIS-SNAPSHOT 1 0\n0\n"},
      {"unsupported version", "CACHEX-SNAPSHOT 99 0\n0\n"},
      {"missing count", "CACHEX-SNAPSHOT 1 0\n"},
      {"negative count", "CACHEX-SNAPSHOT 1 0\n-5\n"},
      {"count exceeds entries", "CACHEX-SNAPSHOT 1 0\n3\n1 1 -1\nab\n"},
      {"truncated payload", "CACHEX-SNAPSHOT 1 0\n1\n10 10 -1\nshort\n"},
      {"negative length", "CACHEX-SNAPSHOT 1 0\n1\n-1 2 -1\nab\n"},
      {"length disagrees with payload", "CACHEX-SNAPSHOT 1 0\n1\n1 1 -1\nabcd\n"},
      {"garbage header", "CACHEX-SNAPSHOT 1 0\n1\nxx yy zz\nab\n"},
  };

  for (const auto& test_case : cases) {
    const TempSnapshot temp("malformed");
    temp.write_raw(test_case.contents);

    cachex::ShardedCache cache(2);
    const auto loaded = cachex::PersistenceManager(temp.path()).load(cache);
    CHECK(!loaded.ok);
    CHECK(!loaded.error.empty());
  }
}

// The load either happens completely or not at all: a truncated file must not
// leave the cache half populated.
CACHEX_TEST(a_malformed_snapshot_leaves_the_cache_untouched) {
  const TempSnapshot temp("atomic_load");
  // Two valid records followed by a truncated third.
  temp.write_raw(
      "CACHEX-SNAPSHOT 1 0\n3\n1 1 -1\nab\n1 1 -1\ncd\n5 5 -1\ntrunc");

  cachex::ShardedCache cache(2);
  cache.set("pre-existing", "value");

  const auto loaded = cachex::PersistenceManager(temp.path()).load(cache);
  CHECK(!loaded.ok);

  // Nothing from the file was applied, and what was already there survived.
  CHECK_EQ(cache.size(), 1u);
  CHECK(cache.contains("pre-existing"));
  CHECK(!cache.contains("a"));
}

CACHEX_TEST(an_empty_file_is_rejected_rather_than_treated_as_empty) {
  const TempSnapshot temp("emptyfile");
  temp.write_raw("");

  cachex::ShardedCache cache(2);
  const auto loaded = cachex::PersistenceManager(temp.path()).load(cache);
  // A zero-byte file is a truncated file, not an empty cache -- treating it as
  // "successfully loaded nothing" would hide a real failure.
  CHECK(!loaded.ok);
}

// --- atomic replacement ----------------------------------------------------

CACHEX_TEST(a_failed_save_leaves_the_previous_snapshot_intact) {
  const TempSnapshot temp("atomic_save");
  cachex::ShardedCache cache(2);
  cache.set("original", "data");
  CHECK(cachex::PersistenceManager(temp.path()).save(cache).ok);
  const std::string good_snapshot = temp.read_raw();

  // A path that cannot be written: the temp file cannot be created, so the
  // rename never happens.
  const cachex::PersistenceManager broken("/nonexistent-directory/snap.cxs");
  const auto failed = broken.save(cache);
  CHECK(!failed.ok);
  CHECK(!failed.error.empty());

  // The real snapshot is byte-for-byte what it was.
  CHECK_EQ(temp.read_raw(), good_snapshot);
}

CACHEX_TEST(saving_twice_replaces_the_snapshot_and_leaves_no_temp_file) {
  const TempSnapshot temp("replace");
  cachex::ShardedCache cache(2);
  cache.set("first", "1");
  const cachex::PersistenceManager manager(temp.path());
  CHECK(manager.save(cache).ok);

  cache.set("second", "2");
  const auto second = manager.save(cache);
  CHECK(second.ok);
  CHECK_EQ(second.entries, 2u);

  std::ifstream leftover(temp.path() + ".tmp", std::ios::binary);
  CHECK(!leftover.good());  // the temp file was renamed, not left behind
}

// --- concurrency -----------------------------------------------------------

// A save runs while other threads are hammering the cache. The snapshot must
// still be well-formed and loadable -- never a torn record.
CACHEX_TEST(a_snapshot_taken_during_concurrent_access_is_still_valid) {
  const TempSnapshot temp("concurrent");
  cachex::ShardedCache cache(8);
  for (int i = 0; i < 2000; ++i) {
    cache.set("stable" + std::to_string(i), "value" + std::to_string(i));
  }

  const cachex::PersistenceManager manager(temp.path());
  std::atomic<bool> stop{false};
  std::atomic<int> saves{0};
  std::atomic<int> failed_saves{0};

  std::vector<std::thread> workers;
  for (int w = 0; w < 6; ++w) {
    workers.emplace_back([&cache, &stop, w] {
      int i = 0;
      while (!stop.load()) {
        const std::string key = "churn" + std::to_string(w) + ":" +
                                std::to_string(i % 500);
        cache.set(key, "v");
        cache.get(key);
        if (i % 3 == 0) {
          cache.erase(key);
        }
        ++i;
      }
    });
  }

  for (int i = 0; i < 8; ++i) {
    if (manager.save(cache).ok) {
      saves.fetch_add(1);
    } else {
      failed_saves.fetch_add(1);
    }
  }

  stop.store(true);
  for (std::thread& worker : workers) {
    worker.join();
  }

  CHECK_EQ(saves.load(), 8);
  CHECK_EQ(failed_saves.load(), 0);

  // The last snapshot must parse cleanly and contain every key that was never
  // touched by the churning threads.
  cachex::ShardedCache restored(8);
  const auto loaded = manager.load(restored);
  CHECK(loaded.ok);

  bool all_stable_present = true;
  for (int i = 0; i < 2000; ++i) {
    if (restored.get("stable" + std::to_string(i)).value_or("") !=
        "value" + std::to_string(i)) {
      all_stable_present = false;
    }
  }
  CHECK(all_stable_present);
}

CACHEX_TEST(concurrent_saves_do_not_corrupt_each_other) {
  const TempSnapshot temp("concurrent_save");
  cachex::ShardedCache cache(4);
  for (int i = 0; i < 500; ++i) {
    cache.set("k" + std::to_string(i), "v");
  }

  // Several threads saving to the same path at once. Each writes its own temp
  // file and renames; the last rename wins, and the result is always one
  // complete snapshot rather than a mixture.
  std::atomic<int> ok_count{0};
  std::vector<std::thread> savers;
  for (int i = 0; i < 4; ++i) {
    savers.emplace_back([&temp, &cache, &ok_count] {
      const cachex::PersistenceManager manager(temp.path());
      if (manager.save(cache).ok) {
        ok_count.fetch_add(1);
      }
    });
  }
  for (std::thread& saver : savers) {
    saver.join();
  }

  cachex::ShardedCache restored(4);
  const auto loaded = cachex::PersistenceManager(temp.path()).load(restored);
  CHECK(loaded.ok);
  CHECK_EQ(restored.size(), 500u);
}

// --- periodic saver --------------------------------------------------------

CACHEX_TEST(the_periodic_saver_writes_on_its_interval_and_stops_cleanly) {
  const TempSnapshot temp("periodic");
  cachex::ShardedCache cache(2);
  cache.set("k", "v");
  cachex::PersistenceManager manager(temp.path());

  {
    cachex::PeriodicSaver saver(manager, cache, std::chrono::seconds(1));
    // Nothing written yet -- the first save is one interval away.
    std::this_thread::sleep_for(100ms);
    CHECK_EQ(saver.saves_completed(), 0u);
    // Destructor stops and joins; it must not wait out the full interval.
  }

  CHECK(!temp.file_exists());  // stopped before the first interval elapsed
}

CACHEX_TEST(the_periodic_saver_can_be_stopped_twice_safely) {
  const TempSnapshot temp("periodic_stop");
  cachex::ShardedCache cache(2);
  cachex::PersistenceManager manager(temp.path());

  cachex::PeriodicSaver saver(manager, cache, std::chrono::seconds(60));
  saver.stop();
  saver.stop();  // idempotent; the destructor calls it a third time
  CHECK_EQ(saver.save_failures(), 0u);
}

// --- untrusted sizes -------------------------------------------------------

// Counts and lengths come from disk. A corrupt value must be reported as a bad
// file, not passed to reserve() or resize() where it would throw bad_alloc and
// take the server down on startup or on LOAD.
CACHEX_TEST(an_impossible_entry_count_is_rejected_without_crashing) {
  const TempSnapshot temp("hugecount");
  temp.write_raw("CACHEX-SNAPSHOT 1 0\n999999999999999999\n");

  cachex::ShardedCache cache(2);
  const auto loaded = cachex::PersistenceManager(temp.path()).load(cache);
  CHECK(!loaded.ok);
  CHECK(loaded.error.find("exceeds") != std::string::npos);
  CHECK_EQ(cache.size(), 0u);
}

CACHEX_TEST(an_impossible_record_length_is_rejected_without_crashing) {
  const TempSnapshot temp("hugelength");
  temp.write_raw("CACHEX-SNAPSHOT 1 0\n1\n999999999999 1 -1\nab\n");

  cachex::ShardedCache cache(2);
  const auto loaded = cachex::PersistenceManager(temp.path()).load(cache);
  CHECK(!loaded.ok);
  CHECK(loaded.error.find("exceeds") != std::string::npos);
  CHECK_EQ(cache.size(), 0u);
}

// The bug this pins: every save used to write the same temp file, so two saves
// at once could interleave their bytes and rename a corrupt snapshot into place.
// A reader loading continuously while several threads save through one manager
// must never see a file that fails to parse.
CACHEX_TEST(concurrent_saves_through_one_manager_never_expose_a_corrupt_snapshot) {
  const TempSnapshot temp("shared_manager");
  cachex::ShardedCache cache(4);
  for (int i = 0; i < 300; ++i) {
    cache.set("stable" + std::to_string(i), std::string(200, 'x'));
  }
  const cachex::PersistenceManager manager(temp.path());
  CHECK(manager.save(cache).ok);  // so the reader never sees "missing"

  std::atomic<bool> stop{false};
  std::atomic<int> failed_saves{0};
  std::atomic<int> failed_loads{0};
  std::atomic<int> loads{0};

  std::thread reader([&] {
    while (!stop.load()) {
      cachex::ShardedCache restored(4);
      if (!manager.load(restored).ok || restored.size() != 300u) {
        failed_loads.fetch_add(1);
      }
      loads.fetch_add(1);
    }
  });

  std::vector<std::thread> savers;
  for (int s = 0; s < 6; ++s) {
    savers.emplace_back([&] {
      for (int i = 0; i < 20; ++i) {
        if (!manager.save(cache).ok) {
          failed_saves.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& saver : savers) {
    saver.join();
  }
  stop.store(true);
  reader.join();

  CHECK_EQ(failed_saves.load(), 0);
  CHECK_EQ(failed_loads.load(), 0);
  CHECK(loads.load() > 0);
}

