#include <gtest/gtest.h>

#include "OtaReleasePolicy.h"

namespace {

TEST(OtaReleasePolicy, ComparesAo3PatchVersions) {
  EXPECT_GT(ota_release_policy::compareVersions("v1.5.1-ao3.11.1", "1.5.1-ao3.11"), 0);
  EXPECT_GT(ota_release_policy::compareVersions("v1.5.1-ao3.12", "1.5.1-ao3.11.9"), 0);
  EXPECT_LT(ota_release_policy::compareVersions("v1.5.1-ao3.10.3", "1.5.1-ao3.11"), 0);
  EXPECT_EQ(ota_release_policy::compareVersions("v1.5.1-ao3.11", "1.5.1-ao3.11"), 0);
}

TEST(OtaReleasePolicy, ComparesCrossInkCoreBeforeAo3Suffix) {
  EXPECT_GT(ota_release_policy::compareVersions("v1.5.2-ao3.1", "1.5.1-ao3.99"), 0);
  EXPECT_LT(ota_release_policy::compareVersions("v1.5.0-ao3.99", "1.5.1-ao3.1"), 0);
  EXPECT_GT(ota_release_policy::compareVersions("v1.5.1-ao3.11", "1.5.1"), 0);
}

TEST(OtaReleasePolicy, PrefersStableVersionOverMatchingReleaseCandidate) {
  EXPECT_GT(ota_release_policy::compareVersions("v1.5.1-ao3.12", "1.5.1-ao3.12-rc1"), 0);
  EXPECT_LT(ota_release_policy::compareVersions("v1.5.1-ao3.12-rc1", "1.5.1-ao3.12"), 0);
}

TEST(OtaReleasePolicy, RejectsInvalidVersions) {
  EXPECT_EQ(ota_release_policy::compareVersions("ao3.12", "1.5.1-ao3.11"), 0);
  EXPECT_EQ(ota_release_policy::compareVersions("1.5.1-ao3.invalid", "1.5.1-ao3.11"), 0);
}

TEST(OtaReleasePolicy, MatchesCanonicalAndAo3X3X4Assets) {
  EXPECT_TRUE(ota_release_policy::matchesFirmwareAsset("firmware-x3-x4.bin", "x3-x4"));
  EXPECT_TRUE(
      ota_release_policy::matchesFirmwareAsset("firmware-x3-x4-v1.5.1-ao3.11.1.bin", "x3-x4"));
  EXPECT_TRUE(
      ota_release_policy::matchesFirmwareAsset("CrossInk-AO3-v1.5.1-ao3.11.1-x3-x4.bin", "x3-x4"));
}

TEST(OtaReleasePolicy, RejectsWrongBoardAndNonFirmwareAssets) {
  EXPECT_FALSE(
      ota_release_policy::matchesFirmwareAsset("CrossInk-AO3-v1.5.1-ao3.11.1-x4-pro.bin", "x3-x4"));
  EXPECT_FALSE(
      ota_release_policy::matchesFirmwareAsset("CrossInk-AO3-v1.5.1-ao3.11.1-source.zip", "x3-x4"));
  EXPECT_FALSE(ota_release_policy::matchesFirmwareAsset("firmware-x3-x4evil-v1.5.1.bin", "x3-x4"));
  EXPECT_FALSE(ota_release_policy::matchesFirmwareAsset("firmware-x4-pro-v1.5.1.bin", "x3-x4"));
}

}  // namespace
