#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

#include "cachex/cache.hpp"
#include "cachex/version.hpp"

namespace {

void print(const char* label, const std::optional<std::string>& value) {
  std::cout << "  " << label << " -> " << (value ? *value : "(nil)") << "\n";
}

void print_order(const cachex::Cache& cache) {
  std::cout << "  order (newest first): ";
  for (const std::string& key : cache.keys_by_recency()) {
    std::cout << key << " ";
  }
  std::cout << "\n";
}

}  // namespace

// There is no server yet, so the executable demonstrates the cache API rather
// than running a process. Stage 6 replaces this with an accept loop.
int main() {
  std::cout << "CacheX " << cachex::version_string()
            << " -- in-process cache demo (no server yet)\n";

  std::cout << "\n[1] Basic operations (unbounded)\n";
  cachex::Cache cache;
  cache.set("user:1", "ada");
  cache.set("user:2", "grace");
  cache.set("user:1", "ada lovelace");  // overwrite

  print("get user:1 ", cache.get("user:1"));
  print("get user:42", cache.get("user:42"));
  std::cout << "  erase user:2 -> " << std::boolalpha << cache.erase("user:2")
            << ", again -> " << cache.erase("user:2")
            << ", size -> " << cache.size() << "\n";

  std::cout << "\n[2] LRU eviction (capacity 3)\n";
  cachex::Cache lru(3);
  lru.set("a", "1");
  lru.set("b", "2");
  lru.set("c", "3");
  print_order(lru);

  std::cout << "  get(\"a\") -- reading it makes it the newest\n";
  lru.get("a");
  print_order(lru);

  std::cout << "  set(\"d\") -- at capacity, so the oldest (\"b\") is evicted\n";
  lru.set("d", "4");
  print_order(lru);

  std::cout << "  contains b -> " << lru.contains("b")
            << ", contains a -> " << lru.contains("a")
            << ", evictions -> " << lru.evictions() << "\n";

  std::cout << "\n[3] TTL (lazy expiration)\n";
  using namespace std::chrono_literals;
  cachex::Cache ttl_cache;
  ttl_cache.set("permanent", "stays");
  ttl_cache.set("fleeting", "goes", 50ms);

  const auto describe = [&ttl_cache](const char* key) {
    const cachex::TtlInfo info = ttl_cache.ttl(key);
    std::cout << "  ttl " << key << " -> ";
    switch (info.state) {
      case cachex::TtlState::Missing:
        std::cout << "missing\n";
        break;
      case cachex::TtlState::Persistent:
        std::cout << "no expiry\n";
        break;
      case cachex::TtlState::Expiring:
        std::cout << info.remaining.count() << " ms left\n";
        break;
    }
  };
  describe("permanent");
  describe("fleeting");

  std::cout << "  ... waiting 150 ms ...\n";
  std::this_thread::sleep_for(150ms);
  describe("permanent");
  describe("fleeting");
  std::cout << "  size -> " << ttl_cache.size() << ", expired removals -> "
            << ttl_cache.expired_removals() << "\n";
  return 0;
}
