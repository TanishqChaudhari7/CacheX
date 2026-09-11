#pragma once

#include <string>

#include "cachex/sharded_cache.hpp"
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
/// Takes a ShardedCache, not a Cache, so the type system rules out handing the
/// server an unsynchronised cache. ShardedCache exposes exactly the same API as
/// SyncCache, so nothing here knows or cares how many shards there are --
/// changing the shard count does not touch a line of the network layer.
std::string execute(ShardedCache& cache, const Command& command);

}  // namespace cachex
