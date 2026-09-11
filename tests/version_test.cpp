#include "cachex/version.hpp"

#include <string>

#include "test_framework.hpp"

// These tests exist to prove the harness works end to end -- library links,
// generated header is found, failures would surface -- not because
// version_string() is interesting.

CACHEX_TEST(version_string_matches_version_constants) {
  const std::string expected = std::to_string(cachex::kVersionMajor) + "." +
                               std::to_string(cachex::kVersionMinor) + "." +
                               std::to_string(cachex::kVersionPatch);
  CHECK_EQ(cachex::version_string(), expected);
}

CACHEX_TEST(version_string_has_three_components) {
  const std::string version = cachex::version_string();
  CHECK(!version.empty());

  int dots = 0;
  for (const char c : version) {
    if (c == '.') {
      ++dots;
    }
  }
  CHECK_EQ(dots, 2);
}
