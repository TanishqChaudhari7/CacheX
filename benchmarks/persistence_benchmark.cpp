#include <cstdio>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "bench_util.hpp"
#include "cachex/persistence.hpp"
#include "cachex/sharded_cache.hpp"

namespace {

using bench::Clock;
using bench::Nanos;

constexpr std::size_t kKeyBytes = 16;
constexpr std::size_t kValueBytes = 64;
constexpr int kRepeats = 5;

std::string pad_key(std::size_t i) {
  const std::string digits = std::to_string(i);
  return "key:" + std::string(12 - digits.size(), '0') + digits;
}

/// A snapshot path that removes itself.
class TempFile {
 public:
  explicit TempFile(const char* name)
      : path_(std::string("/tmp/cachex_bench_") + name + ".cxs") {
    std::remove(path_.c_str());
  }
  ~TempFile() {
    std::remove(path_.c_str());
    std::remove((path_ + ".tmp").c_str());
  }
  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

struct Row {
  std::size_t entries = 0;
  double save_ms = 0.0;
  double load_ms = 0.0;
  std::size_t bytes = 0;
};

Row measure(std::size_t entries, bool with_ttl) {
  TempFile temp(with_ttl ? "ttl" : "plain");
  const cachex::PersistenceManager manager(temp.path());
  const std::string value(kValueBytes, 'v');

  cachex::ShardedCache cache(8);
  for (std::size_t i = 0; i < entries; ++i) {
    if (with_ttl) {
      cache.set(pad_key(i), value, std::chrono::hours(1));
    } else {
      cache.set(pad_key(i), value);
    }
  }

  Row row;
  row.entries = entries;

  // Median of several runs: a single disk write is at the mercy of whatever
  // else the filesystem is doing.
  std::vector<Nanos> saves;
  std::vector<Nanos> loads;
  for (int i = 0; i < kRepeats; ++i) {
    const auto save_start = Clock::now();
    const cachex::PersistenceManager::SaveResult saved = manager.save(cache);
    saves.push_back(std::chrono::duration_cast<Nanos>(Clock::now() - save_start));
    if (!saved.ok) {
      std::cerr << "benchmark: save failed: " << saved.error << "\n";
      return row;
    }
    row.bytes = saved.bytes;

    cachex::ShardedCache restored(8);
    const auto load_start = Clock::now();
    const cachex::PersistenceManager::LoadResult loaded = manager.load(restored);
    loads.push_back(std::chrono::duration_cast<Nanos>(Clock::now() - load_start));
    if (!loaded.ok) {
      std::cerr << "benchmark: load failed: " << loaded.error << "\n";
      return row;
    }
  }

  row.save_ms = static_cast<double>(bench::median(saves).count()) / 1e6;
  row.load_ms = static_cast<double>(bench::median(loads).count()) / 1e6;
  return row;
}

void print_rows(const char* title, const std::vector<Row>& rows) {
  std::cout << "\n" << title << "\n\n"
            << std::left << std::setw(12) << "entries" << std::right
            << std::setw(12) << "save (ms)" << std::setw(12) << "load (ms)"
            << std::setw(14) << "save us/entry" << std::setw(14) << "snapshot KiB"
            << std::setw(14) << "bytes/entry" << "\n"
            << std::string(78, '-') << "\n";
  for (const Row& row : rows) {
    const double per_entry_us =
        row.entries > 0 ? row.save_ms * 1000.0 / static_cast<double>(row.entries)
                        : 0.0;
    std::cout << std::left << std::setw(12) << row.entries << std::right
              << std::setw(12) << std::fixed << std::setprecision(2) << row.save_ms
              << std::setw(12) << row.load_ms << std::setw(14)
              << std::setprecision(3) << per_entry_us << std::setw(14)
              << std::setprecision(1)
              << static_cast<double>(row.bytes) / 1024.0 << std::setw(14)
              << std::setprecision(1)
              << (row.entries > 0 ? static_cast<double>(row.bytes) /
                                        static_cast<double>(row.entries)
                                  : 0.0)
              << "\n";
  }
}

}  // namespace

int main() {
  std::cout << "CacheX persistence benchmark\n"
            << "============================\n";

#ifndef NDEBUG
  std::cout << "\n*** WARNING: assertions are enabled -- this is not a Release\n"
            << "*** build. These numbers measure the absence of the optimiser.\n";
#endif

  std::cout << "\nbuild        : " << CACHEX_BUILD_TYPE << "\n"
            << "compiler     : " << CACHEX_COMPILER << "\n"
            << "key size     : " << kKeyBytes << " bytes\n"
            << "value size   : " << kValueBytes << " bytes\n"
            << "shards       : 8\n"
            << "repeats      : " << kRepeats << ", median reported\n"
            << "storage      : /tmp on this machine's filesystem\n";

  const std::vector<std::size_t> sizes = {1000, 10000, 100000, 500000};

  std::vector<Row> plain;
  std::vector<Row> with_ttl;
  for (const std::size_t entries : sizes) {
    plain.push_back(measure(entries, /*with_ttl=*/false));
    with_ttl.push_back(measure(entries, /*with_ttl=*/true));
  }

  print_rows("Entries without a TTL", plain);
  print_rows("Entries with a TTL", with_ttl);

  // How the on-disk size compares with what the same data costs in memory.
  std::cout << "\n\nSNAPSHOT SIZE vs IN-MEMORY SIZE\n\n";
  const std::size_t payload = kKeyBytes + kValueBytes;
  const Row& reference = plain.back();
  const double snapshot_per_entry =
      reference.entries > 0
          ? static_cast<double>(reference.bytes) /
                static_cast<double>(reference.entries)
          : 0.0;

  std::cout << "  payload (key + value)        : " << payload << " bytes/entry\n"
            << "  snapshot                     : " << std::fixed
            << std::setprecision(1) << snapshot_per_entry << " bytes/entry ("
            << std::setprecision(2) << snapshot_per_entry / static_cast<double>(payload)
            << "x payload)\n"
            << "  in memory (estimated)        : ~180-200 bytes/entry ("
            << std::setprecision(1) << 190.0 / static_cast<double>(payload)
            << "x payload)\n\n"
            << "  The snapshot is far smaller than the live cache because it\n"
            << "  stores only the data. The hash map's buckets and nodes, both\n"
            << "  list pointers, the duplicated key and the allocator's overhead\n"
            << "  all exist to make lookups O(1) -- none of that is worth writing\n"
            << "  down, because loading rebuilds it.\n"
            << "\n  The in-memory figure is an estimate from the data layout, not\n"
            << "  a measurement -- see ARCHITECTURE.md.\n";

  return 0;
}
