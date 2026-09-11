#include <iostream>
#include <optional>
#include <string>

#include "cachex/cache.hpp"
#include "cachex/version.hpp"

namespace {

void print(const char* label, const std::optional<std::string>& value) {
  std::cout << "  " << label << " -> " << (value ? *value : "(nil)") << "\n";
}

}  // namespace

// There is no server yet, so the executable is a short demonstration of the
// cache API rather than a running process. Stage 6 replaces this with an
// accept loop.
int main() {
  std::cout << "CacheX " << cachex::version_string()
            << " -- in-process cache demo (no server yet)\n\n";

  cachex::Cache cache;
  cache.set("user:1", "ada");
  cache.set("user:2", "grace");
  cache.set("user:1", "ada lovelace");  // overwrite

  print("get user:1 ", cache.get("user:1"));
  print("get user:2 ", cache.get("user:2"));
  print("get user:42", cache.get("user:42"));

  std::cout << "  contains user:2 -> " << std::boolalpha << cache.contains("user:2")
            << "\n  erase user:2    -> " << cache.erase("user:2")
            << "\n  erase user:2    -> " << cache.erase("user:2")
            << "\n  size            -> " << cache.size() << "\n";
  return 0;
}
