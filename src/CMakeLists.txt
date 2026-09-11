# Expand @PROJECT_VERSION_*@ into a real header inside the build directory.
# Generated headers live in the build tree, never in the source tree, so a
# clean checkout is never polluted by build output.
configure_file(
  "${PROJECT_SOURCE_DIR}/include/cachex/version.hpp.in"
  "${PROJECT_BINARY_DIR}/generated/cachex/version.hpp"
  @ONLY)

# The real code lives in a library; the executable is a thin main() on top.
# This is what lets the tests link against the same code the server runs,
# instead of the tests rebuilding their own copy of it.
add_library(cachex_core
  version.cpp)

target_include_directories(cachex_core PUBLIC
  "${PROJECT_SOURCE_DIR}/include"
  "${PROJECT_BINARY_DIR}/generated")

# PRIVATE: warning flags are our build's business, not part of the library's
# public interface.
target_link_libraries(cachex_core PRIVATE cachex_warnings)

add_executable(cachex
  main.cpp)

target_link_libraries(cachex PRIVATE cachex_core cachex_warnings)
