#include "cachex/persistence.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace cachex {
namespace {

// The format, version 1:
//
//   CACHEX-SNAPSHOT 1 <saved_at_unix_ms>\n
//   <entry_count>\n
//   repeated entry_count times:
//     <key_len> <value_len> <ttl_ms>\n
//     <key bytes><value bytes>\n
//
// ttl_ms is -1 for an entry that never expires, otherwise the milliseconds
// remaining *at the moment of the save*.
//
// That is why the header carries a wall-clock timestamp. TTLs are tracked on
// steady_clock, which is monotonic but whose epoch is meaningless outside the
// process -- so the file cannot store deadlines, only durations. A duration
// alone is not enough either: a key saved with 60 ms left would come back with
// a fresh 60 ms no matter how long the server was down. The loader needs to
// know how much wall-clock time passed, so system_clock is used for exactly
// that one job and nothing else.
//
// Lengths come first and the payload is copied verbatim, so a key or value may
// contain spaces, newlines or NUL bytes -- everything the cache can actually
// hold. This is the length-prefixed framing that docs/PROTOCOL.md points at as
// the fix for the wire protocol's whitespace limitation; the snapshot gets it
// because nothing here has to be typed by a human.
constexpr const char* kMagic = "CACHEX-SNAPSHOT";
constexpr int kVersion = 1;
constexpr long long kNoExpiry = -1;

/// Milliseconds since the Unix epoch. Used only to measure how long the file sat
/// on disk, never to decide whether an entry is expired while running.
long long wall_clock_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

/// One record as read back off disk, before anything is applied to the cache.
struct Record {
  std::string key;
  std::string value;
  long long ttl_ms = kNoExpiry;
};

bool read_exact(std::istream& in, std::string& out, std::size_t count) {
  out.resize(count);
  if (count == 0) {
    return true;
  }
  in.read(&out[0], static_cast<std::streamsize>(count));
  return in.gcount() == static_cast<std::streamsize>(count);
}

}  // namespace

bool PersistenceManager::exists() const {
  std::ifstream file(path_, std::ios::binary);
  return file.good();
}

PersistenceManager::SaveResult PersistenceManager::save(
    const ShardedCache& cache) const {
  SaveResult result;

  // Read the cache first, then write. The file is never open while a shard lock
  // is held, so a slow disk cannot stall the request path.
  const std::vector<EntrySnapshot> entries = cache.export_entries();

  const std::string temp_path = path_ + ".tmp";
  {
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
      result.error = "cannot open '" + temp_path + "' for writing";
      return result;
    }

    out << kMagic << " " << kVersion << " " << wall_clock_ms() << "\n"
        << entries.size() << "\n";
    for (const EntrySnapshot& entry : entries) {
      const long long ttl =
          entry.remaining_ttl.has_value()
              ? static_cast<long long>(entry.remaining_ttl->count())
              : kNoExpiry;
      out << entry.key.size() << " " << entry.value.size() << " " << ttl << "\n";
      out.write(entry.key.data(), static_cast<std::streamsize>(entry.key.size()));
      out.write(entry.value.data(),
                static_cast<std::streamsize>(entry.value.size()));
      out << "\n";
    }
    out.flush();
    if (!out) {
      out.close();
      std::remove(temp_path.c_str());
      result.error = "write failed (disk full?)";
      return result;
    }
    result.bytes = static_cast<std::size_t>(out.tellp());
  }

  // Atomic replace. Before this line the old snapshot is still the real one;
  // after it, the new one is. There is no instant where neither is valid.
  if (std::rename(temp_path.c_str(), path_.c_str()) != 0) {
    const std::string reason = std::strerror(errno);
    std::remove(temp_path.c_str());
    result.error = "cannot rename snapshot into place: " + reason;
    return result;
  }

  result.ok = true;
  result.entries = entries.size();
  return result;
}

PersistenceManager::LoadResult PersistenceManager::load(ShardedCache& cache) const {
  LoadResult result;

  std::ifstream in(path_, std::ios::binary);
  if (!in) {
    // Not an error: a server starting for the first time has no snapshot.
    result.ok = true;
    return result;
  }

  std::string magic;
  int version = 0;
  long long saved_at_ms = 0;
  if (!(in >> magic >> version >> saved_at_ms) || magic != kMagic) {
    result.error = "not a CacheX snapshot";
    return result;
  }
  if (version != kVersion) {
    result.error = "unsupported snapshot version " + std::to_string(version);
    return result;
  }

  // How long the snapshot sat on disk. Clamped at zero: if the wall clock moved
  // backwards between the save and now (NTP, a manual change), the elapsed time
  // would come out negative and would *extend* every TTL. Refusing to go
  // negative means the worst a clock jump can do is keep a key slightly too
  // long, never resurrect one that should be gone.
  const long long elapsed_ms = std::max<long long>(0, wall_clock_ms() - saved_at_ms);

  long long count = 0;
  if (!(in >> count) || count < 0) {
    result.error = "missing or invalid entry count";
    return result;
  }
  in.get();  // the newline after the count

  // Everything is parsed and validated before a single entry is applied, so a
  // truncated or corrupt file leaves the cache exactly as it was.
  std::vector<Record> records;
  records.reserve(static_cast<std::size_t>(count));

  for (long long i = 0; i < count; ++i) {
    long long key_len = 0;
    long long value_len = 0;
    long long ttl_ms = 0;
    if (!(in >> key_len >> value_len >> ttl_ms)) {
      result.error = "truncated entry header at record " + std::to_string(i);
      return result;
    }
    if (key_len < 0 || value_len < 0) {
      result.error = "negative length at record " + std::to_string(i);
      return result;
    }
    if (in.get() != '\n') {
      result.error = "malformed entry header at record " + std::to_string(i);
      return result;
    }

    Record record;
    record.ttl_ms = ttl_ms;
    if (!read_exact(in, record.key, static_cast<std::size_t>(key_len)) ||
        !read_exact(in, record.value, static_cast<std::size_t>(value_len))) {
      result.error = "truncated payload at record " + std::to_string(i);
      return result;
    }
    // The trailing newline is redundant given the lengths, which is exactly why
    // it is worth checking: if it is missing, the lengths disagreed with the
    // file and everything after this point would be garbage.
    if (in.get() != '\n') {
      result.error = "payload length mismatch at record " + std::to_string(i);
      return result;
    }
    records.push_back(std::move(record));
  }

  // Applied in reverse: export_entries() lists each shard's entries most
  // recently used first, so replaying backwards makes the most recently used
  // key the last one set -- restoring the same recency order *within each
  // shard*. Across shards the order is not preserved, and cannot be: LRU is
  // per-shard (§9.4), so there is no global order to restore.
  for (auto it = records.rbegin(); it != records.rend(); ++it) {
    if (it->ttl_ms == kNoExpiry) {
      cache.set(std::move(it->key), std::move(it->value));
      ++result.loaded;
      continue;
    }
    // The stored value was the time left when the file was written; subtract
    // however long it has been sitting there.
    const long long remaining_ms = it->ttl_ms - elapsed_ms;
    if (remaining_ms > 0) {
      cache.set(std::move(it->key), std::move(it->value),
                std::chrono::milliseconds(remaining_ms));
      ++result.loaded;
    } else {
      // Ran out while the file was on disk. Restoring it would resurrect a key
      // that is already dead.
      ++result.expired;
    }
  }

  result.ok = true;
  return result;
}

PeriodicSaver::PeriodicSaver(PersistenceManager& manager, ShardedCache& cache,
                             std::chrono::seconds interval)
    : manager_(manager), cache_(cache), interval_(interval) {
  thread_ = std::thread([this] { run(); });
}

PeriodicSaver::~PeriodicSaver() { stop(); }

void PeriodicSaver::stop() {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      return;
    }
    stopping_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void PeriodicSaver::run() {
  while (true) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      // wait_for returns early when stop() notifies, so shutdown does not have
      // to wait out the remainder of the interval.
      wake_.wait_for(lock, interval_, [this] { return stopping_; });
      if (stopping_) {
        return;
      }
    }
    // The lock is released before saving: a save can take a while, and holding
    // it would make stop() wait for the write to finish.
    if (manager_.save(cache_).ok) {
      saves_.fetch_add(1);
    } else {
      failures_.fetch_add(1);
    }
  }
}

}  // namespace cachex
