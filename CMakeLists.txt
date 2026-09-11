cmake_minimum_required(VERSION 3.16)

project(CacheX
  VERSION 0.1.0
  DESCRIPTION "A small Redis-inspired in-memory cache server"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Language standard
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use -std=c++17 rather than -std=gnu++17. GNU extensions compile fine on GCC
# and Clang but would let us depend on them without noticing, and the code
# would then fail on a stricter compiler.
set(CMAKE_CXX_EXTENSIONS OFF)

# Emit compile_commands.json so clangd / clang-tidy work without extra setup.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Build options
# ---------------------------------------------------------------------------
option(CACHEX_BUILD_TESTS "Build the CacheX test suite" ON)
option(CACHEX_BUILD_BENCHMARKS "Build the CacheX benchmarks" OFF)
option(CACHEX_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# Single-config generators (Unix Makefiles, Ninja) leave CMAKE_BUILD_TYPE empty
# by default, which means no optimisation *and* no debug info -- the worst of
# both. Default to Debug so an unconfigured build is still a useful one.
# Multi-config generators (Xcode, Visual Studio) choose at build time instead,
# so they must be left alone.
get_property(cachex_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT cachex_is_multi_config AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")

# ---------------------------------------------------------------------------
# Warning settings
#
# Carried by an INTERFACE target instead of global CMAKE_CXX_FLAGS so that the
# flags apply to our targets only. If we ever pull in a third-party library, its
# warnings are its own problem and should not drown out ours.
# ---------------------------------------------------------------------------
add_library(cachex_warnings INTERFACE)

if(MSVC)
  target_compile_options(cachex_warnings INTERFACE /W4 /permissive-)
  if(CACHEX_WARNINGS_AS_ERRORS)
    target_compile_options(cachex_warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(cachex_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow              # a local hiding a member is a classic silent bug
    -Wconversion          # narrowing int/size_t conversions, very common in caches
    -Wsign-conversion
    -Wnon-virtual-dtor    # deleting through a base pointer without a virtual dtor
    -Woverloaded-virtual  # accidentally hiding a base method instead of overriding
    -Wold-style-cast      # forces C++ casts, which are greppable and checked
    -Wdouble-promotion
    -Wformat=2)
  if(CACHEX_WARNINGS_AS_ERRORS)
    target_compile_options(cachex_warnings INTERFACE -Werror)
  endif()
endif()

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
add_subdirectory(src)

if(CACHEX_BUILD_TESTS)
  # enable_testing() must be called from the top-level list file for `ctest` to
  # see the tests registered in the subdirectory.
  enable_testing()
  add_subdirectory(tests)
endif()

if(CACHEX_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

message(STATUS "CacheX ${PROJECT_VERSION} | C++${CMAKE_CXX_STANDARD} | build type: ${CMAKE_BUILD_TYPE}")
