#include <gtest/gtest.h>

#include "activities/network/Ao3SyncMemoryPolicy.h"

TEST(Ao3SyncMemoryPolicyTest, AcceptsThresholdAndLargerHeaps) {
  EXPECT_TRUE(Ao3SyncMemoryPolicy::hasNetworkHeadroom(Ao3SyncMemoryPolicy::MIN_FREE_HEAP,
                                                      Ao3SyncMemoryPolicy::MIN_MAX_ALLOC_HEAP));
  EXPECT_TRUE(Ao3SyncMemoryPolicy::hasNetworkHeadroom(128U * 1024U, 96U * 1024U));
}

TEST(Ao3SyncMemoryPolicyTest, RejectsLowTotalFreeHeap) {
  EXPECT_FALSE(Ao3SyncMemoryPolicy::hasNetworkHeadroom(Ao3SyncMemoryPolicy::MIN_FREE_HEAP - 1,
                                                       Ao3SyncMemoryPolicy::MIN_MAX_ALLOC_HEAP));
}

TEST(Ao3SyncMemoryPolicyTest, RejectsFragmentedHeap) {
  EXPECT_FALSE(Ao3SyncMemoryPolicy::hasNetworkHeadroom(Ao3SyncMemoryPolicy::MIN_FREE_HEAP * 2,
                                                       Ao3SyncMemoryPolicy::MIN_MAX_ALLOC_HEAP - 1));
}
