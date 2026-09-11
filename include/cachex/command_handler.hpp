#pragma once

#include <string>

#include "cachex/sync_cache.hpp"
#include "cachex/protocol.hpp"

namespace cachex {

/// Applies one parsed command to the cache and returns the wire reply.
///
/// This is the whole bridge between the network and the cache, and it is the
/// only place that knows about both. It takes a Command, not bytes, and returns
/// a string, not a socket -- so the entire request/response behaviour of the
/// server can be tested without opening a single connection.
///
/// The direction of the dependency is the point: networking calls the cache, and
/// the cache knows nothing about any of this.
///
/// Takes a SyncCache, not a Cache, so the type system rules out handing the
/// server an unsynchronised cache.
std::string execute(SyncCache& cache, const Command& command);

}  // namespace cachex
