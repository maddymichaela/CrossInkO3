#include <gtest/gtest.h>

#include "activities/home/Ao3IndexMemoryPolicy.h"

TEST(Ao3IndexMemoryPolicyTest, AcceptsThresholdAndLargerHeaps) {
  EXPECT_TRUE(Ao3IndexMemoryPolicy::hasMetadataHeadroom(Ao3IndexMemoryPolicy::MIN_FREE_HEAP,
                                                        Ao3IndexMemoryPolicy::MIN_MAX_ALLOC_HEAP));
  EXPECT_TRUE(Ao3IndexMemoryPolicy::hasMetadataHeadroom(96U * 1024U, 64U * 1024U));
}

TEST(Ao3IndexMemoryPolicyTest, RejectsLowTotalFreeHeap) {
  EXPECT_FALSE(Ao3IndexMemoryPolicy::hasMetadataHeadroom(Ao3IndexMemoryPolicy::MIN_FREE_HEAP - 1,
                                                         Ao3IndexMemoryPolicy::MIN_MAX_ALLOC_HEAP));
}

TEST(Ao3IndexMemoryPolicyTest, RejectsFragmentedHeap) {
  EXPECT_FALSE(Ao3IndexMemoryPolicy::hasMetadataHeadroom(Ao3IndexMemoryPolicy::MIN_FREE_HEAP * 2,
                                                         Ao3IndexMemoryPolicy::MIN_MAX_ALLOC_HEAP - 1));
}
