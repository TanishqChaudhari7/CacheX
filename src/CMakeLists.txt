# Expand @PROJECT_VERSION_*@ into a real header inside the build directory.
# Generated headers live in the build tree, never in the source tree, so a
# clean checkout is never polluted by build output.
configure_file(
  "${PROJECT_SOURCE_DIR}/include/cachex/version.hpp.in"
  "${PROJECT_BINARY_DIR}/generated/cachex/version.hpp"
  @ONLY)

# --- The cache engine -------------------------------------------------------
#
# Knows nothing about sockets, and must stay that way. The dependency runs one
# way only: networking calls the cache.
find_package(Threads REQUIRED)

add_library(cachex_core
  cache.cpp
  recency_list.cpp
  version.cpp)

target_include_directories(cachex_core PUBLIC
  "${PROJECT_SOURCE_DIR}/include"
  "${PROJECT_BINARY_DIR}/generated")

# PRIVATE: warning flags are our build's business, not part of the library's
# public interface.
# PUBLIC: SyncCache's mutex is part of the public header, so anything using the
# cache concurrently needs the threading library too.
target_link_libraries(cachex_core PUBLIC Threads::Threads)
target_link_libraries(cachex_core PRIVATE cachex_warnings)

# --- The network layer ------------------------------------------------------
#
# A separate target, not just separate files, so the architectural boundary is
# enforced by the build graph: cachex_core cannot accidentally start depending
# on socket code, because it does not link this.
add_library(cachex_net
  net/line_buffer.cpp
  net/protocol.cpp
  net/socket.cpp
  net/command_handler.cpp
  net/connection.cpp
  net/server.cpp)

target_link_libraries(cachex_net PUBLIC cachex_core)
target_link_libraries(cachex_net PRIVATE cachex_warnings)

# --- Executables ------------------------------------------------------------

# In-process demo of the cache API; no networking involved.
add_executable(cachex main.cpp)
target_link_libraries(cachex PRIVATE cachex_core cachex_warnings)

add_executable(cachex_server server_main.cpp)
target_link_libraries(cachex_server PRIVATE cachex_net cachex_warnings)

add_executable(cachex_client client_main.cpp)
target_link_libraries(cachex_client PRIVATE cachex_net cachex_warnings)
