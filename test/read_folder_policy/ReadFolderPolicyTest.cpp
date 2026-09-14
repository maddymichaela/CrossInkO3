#include <gtest/gtest.h>

#include "util/ReadFolderPolicy.h"

TEST(ReadFolderPolicy, UsesReadAsSafeDefault) {
  EXPECT_EQ(ReadFolderPolicy::normalizeFolder(""), "/Read");
  EXPECT_EQ(ReadFolderPolicy::normalizeFolder("/"), "/Read");
}

TEST(ReadFolderPolicy, NormalizesSelectedFolder) {
  EXPECT_EQ(ReadFolderPolicy::normalizeFolder("Finished/Books/"), "/Finished/Books");
  EXPECT_EQ(ReadFolderPolicy::normalizeFolder("//Finished///Books//"), "/Finished/Books");
}

TEST(ReadFolderPolicy, MatchesFolderAndDescendantsOnly) {
  EXPECT_TRUE(ReadFolderPolicy::isPathInFolder("/Finished", "/Finished"));
  EXPECT_TRUE(ReadFolderPolicy::isPathInFolder("/Finished/AO3/fic.epub", "/Finished/"));
  EXPECT_FALSE(ReadFolderPolicy::isPathInFolder("/Finished-ish/fic.epub", "/Finished"));
  EXPECT_FALSE(ReadFolderPolicy::isPathInFolder("/Books/fic.epub", "/Finished"));
}

TEST(ReadFolderPolicy, FastNormalizedCheckUsesSafeDefault) {
  EXPECT_TRUE(ReadFolderPolicy::isPathInNormalizedFolder("/Read/book.epub", ""));
  EXPECT_TRUE(ReadFolderPolicy::isPathInNormalizedFolder("/Finished/book.epub", "/Finished"));
  EXPECT_FALSE(ReadFolderPolicy::isPathInNormalizedFolder("/Books/book.epub", "/Finished"));
}
