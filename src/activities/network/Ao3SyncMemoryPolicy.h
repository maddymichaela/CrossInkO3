#pragma once

#include <cstdint>

namespace Ao3SyncMemoryPolicy {

// Match CrossInk's established KOReader TLS floor. The previous AO3-specific
// limits rejected healthy X3 sessions after Wi-Fi startup, before an HTTPS
// request was even attempted.
constexpr uint32_t MIN_FREE_HEAP = 35000U;
constexpr uint32_t MIN_MAX_ALLOC_HEAP = 20000U;

inline bool hasNetworkHeadroom(const uint32_t freeHeap, const uint32_t maxAllocHeap) {
  return freeHeap >= MIN_FREE_HEAP && maxAllocHeap >= MIN_MAX_ALLOC_HEAP;
}

}  // namespace Ao3SyncMemoryPolicy
