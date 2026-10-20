#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include "Ao3UpdateParser.h"

namespace {
void feed(Ao3UpdateParser& parser, const std::string& html, const size_t chunkSize) {
  for (size_t offset = 0; offset < html.size(); offset += chunkSize) {
    const size_t length = std::min(chunkSize, html.size() - offset);
    parser.feed(reinterpret_cast<const uint8_t*>(html.data() + offset), length);
  }
}
}  // namespace

TEST(Ao3UpdateParser, ParsesCompletedWorkAcrossChunkBoundaries) {
  Ao3UpdateParser parser;
  feed(parser,
       R"(<dl class="stats"><dd class="status">2026-08-15</dd><dd class="chapters">12/12</dd></dl>)", 7);

  ASSERT_TRUE(parser.complete());
  EXPECT_EQ(parser.result().updatedDate, "2026-08-15");
  EXPECT_TRUE(parser.result().isCompleted);
}

TEST(Ao3UpdateParser, ParsesWorkInProgress) {
  Ao3UpdateParser parser;
  feed(parser, R"(<dd class="status"> 2026-08-14 </dd><dd class="chapters">8/?</dd>)", 3);

  ASSERT_TRUE(parser.complete());
  EXPECT_FALSE(parser.result().isCompleted);
}

TEST(Ao3UpdateParser, FallsBackToPublishedDateForOneShot) {
  Ao3UpdateParser parser;
  feed(parser, R"(<dd class="published">2026-01-02</dd><dd class="chapters">1/1</dd>)", 64);

  ASSERT_TRUE(parser.complete());
  EXPECT_EQ(parser.result().updatedDate, "2026-01-02");
  EXPECT_TRUE(parser.result().isCompleted);
}

TEST(Ao3UpdateParser, AcceptsAdditionalAttributesAndClassTokens) {
  Ao3UpdateParser parser;
  feed(parser,
       R"(<dd data-label="Updated" class='status datetime'>2026-10-06</dd>)"
       R"(<dd class="count chapters" data-label="Chapters">5/?</dd>)",
       5);

  ASSERT_TRUE(parser.complete());
  EXPECT_EQ(parser.result().updatedDate, "2026-10-06");
  EXPECT_FALSE(parser.result().isCompleted);
}

TEST(Ao3UpdateParser, SignalsStreamingCompletionAsSoonAsBothFieldsArrive) {
  Ao3UpdateParser parser;
  const std::string html = R"(<dd class="status">2026-10-06</dd><dd class="chapters">5/?</dd>)";

  EXPECT_FALSE(parser.feed(reinterpret_cast<const uint8_t*>(html.data()), html.size()));
  EXPECT_TRUE(parser.complete());
}

TEST(Ao3UpdateParser, RejectsMalformedDateAndChapterCount) {
  Ao3UpdateParser parser;
  feed(parser, R"(<dd class="status">yesterday</dd><dd class="chapters">twelve</dd>)", 8);

  EXPECT_FALSE(parser.complete());
  EXPECT_FALSE(parser.result().hasDate);
  EXPECT_FALSE(parser.result().hasChapterStatus);
}
