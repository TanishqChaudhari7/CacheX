add_executable(cachex_tests
  test_main.cpp
  version_test.cpp)

# The test framework header sits next to the test sources.
target_include_directories(cachex_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(cachex_tests PRIVATE cachex_core cachex_warnings)

# One ctest entry that runs the whole binary. Per-test ctest entries would give
# finer-grained output but would mean re-running the executable once per test;
# the binary already prints per-test results itself.
add_test(NAME cachex_tests COMMAND cachex_tests)
