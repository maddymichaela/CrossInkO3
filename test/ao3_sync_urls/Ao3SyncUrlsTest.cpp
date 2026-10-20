#include <gtest/gtest.h>

#include "activities/network/Ao3SyncUrls.h"

TEST(Ao3SyncUrlsTest, WorkPageAvoidsAdultCookieRedirect) {
  EXPECT_EQ(Ao3SyncUrls::workPage("archiveofourown.org", "90778581"),
            "https://archiveofourown.org/works/90778581?view_full_work=true&view_adult=true");
}

TEST(Ao3SyncUrlsTest, DownloadRetainsExistingEndpoint) {
  EXPECT_EQ(Ao3SyncUrls::download("archiveofourown.org", "90778581", "2026-10-06"),
            "https://archiveofourown.org/downloads/90778581/work.epub?v=2026-10-06");
}
