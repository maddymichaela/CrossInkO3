#pragma once

#include <cstdint>

namespace Ao3SyncMemoryPolicy {

// The X3/X4 secure request path needs one sizeable contiguous internal-heap
// allocation. Refuse to start when fragmentation makes a clean TLS session
// unlikely instead of risking a crash halfway through the operation.
constexpr uint32_t MIN_FREE_HEAP = 64U * 1024U;
constexpr uint32_t MIN_MAX_ALLOC_HEAP = 50U * 1024U;

inline bool hasNetworkHeadroom(const uint32_t freeHeap, const uint32_t maxAllocHeap) {
  return freeHeap >= MIN_FREE_HEAP && maxAllocHeap >= MIN_MAX_ALLOC_HEAP;
}

}  // namespace Ao3SyncMemoryPolicy
