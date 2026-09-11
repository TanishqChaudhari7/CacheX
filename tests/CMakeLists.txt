add_executable(cachex_tests
  test_main.cpp
  cache_test.cpp
  lru_test.cpp
  ttl_test.cpp
  recency_list_test.cpp
  line_buffer_test.cpp
  protocol_test.cpp
  server_test.cpp
  version_test.cpp)

# The test framework header sits next to the test sources.
target_include_directories(cachex_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# The integration tests drive a real Server over a real socket, so they need the
# network layer -- and a thread to run the accept loop in while the test acts as
# the client.
find_package(Threads REQUIRED)
target_link_libraries(cachex_tests PRIVATE cachex_net cachex_warnings Threads::Threads)

# One ctest entry that runs the whole binary. Per-test ctest entries would give
# finer-grained output but would mean re-running the executable once per test;
# the binary already prints per-test results itself.
add_test(NAME cachex_tests COMMAND cachex_tests)
