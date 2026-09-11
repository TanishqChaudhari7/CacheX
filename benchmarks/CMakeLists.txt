add_executable(cachex_bench
  cache_benchmark.cpp)

target_link_libraries(cachex_bench PRIVATE cachex_core cachex_warnings)

# Stamp the build configuration into the binary so a saved result sheet always
# says which build produced it. $<CONFIG> rather than ${CMAKE_BUILD_TYPE} so it
# is also correct under multi-config generators.
target_compile_definitions(cachex_bench PRIVATE
  CACHEX_BUILD_TYPE="$<CONFIG>"
  CACHEX_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
