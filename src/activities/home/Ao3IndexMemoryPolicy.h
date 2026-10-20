#pragma once

#include <cstdint>

namespace Ao3IndexMemoryPolicy {

// Metadata-only EPUB loading can borrow the display framebuffer for ZIP
// inflation, but still needs a modest amount of ordinary heap for parsers and
// index records. These limits match the proven secure-network floor on X3/X4.
constexpr uint32_t MIN_FREE_HEAP = 35U * 1000U;
constexpr uint32_t MIN_MAX_ALLOC_HEAP = 20U * 1000U;

inline bool hasMetadataHeadroom(const uint32_t freeHeap, const uint32_t maxAllocHeap) {
  return freeHeap >= MIN_FREE_HEAP && maxAllocHeap >= MIN_MAX_ALLOC_HEAP;
}

}  // namespace Ao3IndexMemoryPolicy
