/*
    GMCA — small engine utilities (monotonic clock, randomness, peer id).
*/

#pragma once

#include <cstdint>
#include <string>

#include "torrent/types.hpp"

namespace torrent {

/// Monotonic milliseconds (steady_clock) for timeouts/rates.
int64_t nowMs();

/// Fill `n` random bytes (peer id / tracker transaction ids).
std::string randomBytes(size_t n);

/// A 20-byte peer id: `prefix` (Azureus style, e.g. "-GM0001-") + random tail.
std::string makePeerId(const std::string& prefix);

}  // namespace torrent
