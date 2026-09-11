add_executable(cachex_bench
  cache_benchmark.cpp)

target_link_libraries(cachex_bench PRIVATE cachex_core cachex_warnings)

# Stamp the build configuration into the binary so a saved result sheet always
# says which build produced it. $<CONFIG> rather than ${CMAKE_BUILD_TYPE} so it
# is also correct under multi-config generators.
target_compile_definitions(cachex_bench PRIVATE
  CACHEX_BUILD_TYPE="$<CONFIG>"
  CACHEX_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

# The network benchmark runs a real server in a thread and drives it over a
# loopback socket, so it needs the network layer and a thread.
find_package(Threads REQUIRED)
add_executable(cachex_net_bench
  net_benchmark.cpp)

target_link_libraries(cachex_net_bench
  PRIVATE cachex_net cachex_warnings Threads::Threads)

target_compile_definitions(cachex_net_bench PRIVATE
  CACHEX_BUILD_TYPE="$<CONFIG>"
  CACHEX_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
