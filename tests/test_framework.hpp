#pragma once

// A ~100 line test framework.
//
// Why not GoogleTest or Catch2? For the size of this project they would be the
// largest dependency in the tree, and the only features we would use are the
// three below: register a test, assert something, report failures with a
// non-zero exit code. Writing it out also keeps the whole build explainable.
// If the suite ever outgrows this, swapping in Catch2 is a contained change
// because the CACHEX_TEST / CHECK names match what it provides.

#include <cstddef>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace cachex::test {

struct TestCase {
  std::string name;
  std::function<void()> fn;
};

// Function-local statics, not namespace-scope globals: tests in different
// translation units register themselves before main() runs, and the order in
// which those translation units are initialised is unspecified. A local static
// is guaranteed to be constructed on first use, so the registry always exists
// by the time the first registrar touches it.
inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

inline std::size_t& failure_count() {
  static std::size_t count = 0;
  return count;
}

struct Registrar {
  Registrar(std::string name, std::function<void()> fn) {
    registry().push_back(TestCase{std::move(name), std::move(fn)});
  }
};

inline void report_failure(const char* file, int line, const std::string& what) {
  ++failure_count();
  std::cerr << "    " << file << ":" << line << ": " << what << "\n";
}

inline int run_all() {
  std::size_t failed = 0;

  for (const TestCase& test : registry()) {
    const std::size_t before = failure_count();
    std::cout << "[ RUN      ] " << test.name << "\n";

    // A throwing test is a failing test, not a crashed run: the remaining
    // tests still need to report.
    try {
      test.fn();
    } catch (const std::exception& e) {
      report_failure("<exception>", 0, std::string("threw: ") + e.what());
    } catch (...) {
      report_failure("<exception>", 0, "threw a non-std exception");
    }

    if (failure_count() == before) {
      std::cout << "[       OK ] " << test.name << "\n";
    } else {
      std::cout << "[  FAILED  ] " << test.name << "\n";
      ++failed;
    }
  }

  const std::size_t total = registry().size();
  std::cout << "\n" << (total - failed) << " / " << total << " tests passed\n";

  // ctest treats any non-zero exit code as a failed test.
  return failed == 0 ? 0 : 1;
}

}  // namespace cachex::test

// The forward declaration lets the registrar name the function before its body
// is written, so the macro can be used as if it were a normal definition:
//
//   CACHEX_TEST(my_test) { CHECK(1 + 1 == 2); }
#define CACHEX_TEST(test_name)                                       \
  static void test_name();                                           \
  static const ::cachex::test::Registrar cachex_registrar_##test_name{ \
      #test_name, test_name};                                        \
  static void test_name()

#define CHECK(expr)                                                          \
  do {                                                                       \
    if (!(expr)) {                                                           \
      ::cachex::test::report_failure(__FILE__, __LINE__,                     \
                                     "CHECK(" #expr ") is false");           \
    }                                                                        \
  } while (false)

#define CHECK_EQ(lhs, rhs)                                                   \
  do {                                                                       \
    const auto& cachex_lhs = (lhs);                                          \
    const auto& cachex_rhs = (rhs);                                          \
    if (!(cachex_lhs == cachex_rhs)) {                                       \
      std::ostringstream cachex_msg;                                         \
      cachex_msg << "CHECK_EQ(" #lhs ", " #rhs ") failed: " << cachex_lhs    \
                 << " != " << cachex_rhs;                                    \
      ::cachex::test::report_failure(__FILE__, __LINE__, cachex_msg.str());  \
    }                                                                        \
  } while (false)
