#include <Epub.h>
#include <HalStorage.h>
#include <RecentBooksStore.h>
#include <activities/home/VirtualViews.h>
#include <activities/reader/BookReadingStats.h>
#include <gtest/gtest.h>

#include <limits>
#include <memory>

namespace {
uint16_t date(unsigned year, unsigned month, unsigned day) {
  return static_cast<uint16_t>(((year - 1980) << 9) | (month << 5) | day);
}
uint16_t time(unsigned hour, unsigned minute, unsigned second = 0) {
  return static_cast<uint16_t>((hour << 11) | (minute << 5) | (second / 2));
}
class VirtualViewsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    fixture::reset();
    fixture::stats.clear();
    RECENT_BOOKS.books.clear();
    fixture::add("/", true);
  }
  void TearDown() override {
    EXPECT_EQ(fixture::overlappingHandles, 0u);
    for (const auto& [path, count] : fixture::handles) EXPECT_EQ(count, 0u) << path;
  }
  void completed(const std::string& path, ReadingStatsDate finishedDate = {}) {
    const auto cache = Epub::cachePathForFilePath(path, "/.crosspoint");
    fixture::add(cache, true);
    fixture::stats[cache] = {true, finishedDate};
  }
  void percent(const std::string& path, unsigned basisPoints) {
    const auto cache = Epub::cachePathForFilePath(path, "/.crosspoint");
    fixture::add(cache, true);
    const auto progress = cache + "/progress_percent.bin";
    fixture::add(progress);
    fixture::files[progress].data = {
        0x50, 0x52, 0x50, 0x45, 1, static_cast<uint8_t>(basisPoints), static_cast<uint8_t>(basisPoints >> 8)};
  }
};
TEST_F(VirtualViewsTest, ValidatesDatesIncludingLeapCenturiesAndTimeFields) {
  EXPECT_NE(VirtualViews::fatTimestamp(date(2000, 2, 29), time(23, 59, 58)), 0u);
  EXPECT_EQ(VirtualViews::fatTimestamp(date(2100, 2, 29), 0), 0u);
  EXPECT_EQ(VirtualViews::fatTimestamp(date(2026, 4, 31), 0), 0u);
  EXPECT_EQ(VirtualViews::fatTimestamp(date(2026, 0, 1), 0), 0u);
  EXPECT_EQ(VirtualViews::fatTimestamp(date(2026, 13, 1), 0), 0u);
  EXPECT_EQ(VirtualViews::fatTimestamp(date(2026, 1, 0), 0), 0u);
  EXPECT_EQ(VirtualViews::fatTimestamp(date(2026, 1, 1), time(24, 0)), 0u);
  EXPECT_EQ(VirtualViews::fatTimestamp(date(2026, 1, 1), time(0, 60)), 0u);
  EXPECT_EQ(VirtualViews::fatTimestamp(date(2026, 1, 1), time(0, 0, 60)), 0u);
  EXPECT_GT(VirtualViews::fatTimestamp(date(2026, 1, 2), 0),
            VirtualViews::fatTimestamp(date(2026, 1, 1), time(23, 59, 58)));
}
TEST_F(VirtualViewsTest, RejectsInvalidProgressRatherThanClampingItIntoFinished) {
  EXPECT_FALSE(VirtualViews::isFinished(false, 99.49f));
  EXPECT_TRUE(VirtualViews::isFinished(false, 99.5f));
  EXPECT_TRUE(VirtualViews::isFinished(false, 100.0f));
  EXPECT_FALSE(VirtualViews::isFinished(false, 100.01f));
  EXPECT_FALSE(VirtualViews::isFinished(false, -1));
  EXPECT_FALSE(VirtualViews::isFinished(false, std::numeric_limits<float>::quiet_NaN()));
  EXPECT_FALSE(VirtualViews::isFinished(false, std::numeric_limits<float>::infinity()));
  EXPECT_TRUE(VirtualViews::isFinished(true, -1));
}
TEST_F(VirtualViewsTest, KeepsTop18RegardlessOfDiscoveryOrder) {
  auto candidates = std::make_unique<VirtualViews::Candidates>(100);
  for (unsigned i = 1; i <= 40; ++i) candidates->add(("/book" + std::to_string(i)).c_str(), i);
  ASSERT_EQ(candidates->size(), 18u);
  EXPECT_EQ((*candidates)[0].timestamp, 40u);
  EXPECT_EQ((*candidates)[17].timestamp, 23u);
  EXPECT_FALSE(candidates->add("/book40", 100));
  EXPECT_FALSE(candidates->add(std::string(VirtualViews::PATH_CAPACITY, 'x').c_str(), 100));
}
TEST_F(VirtualViewsTest, SortsUnknownAndTiedDatesByPath) {
  auto candidates = std::make_unique<VirtualViews::Candidates>(4);
  candidates->add("/z", 0);
  candidates->add("/c", 8);
  candidates->add("/b", 8);
  candidates->add("/a", 0);
  ASSERT_EQ(candidates->size(), 4u);
  EXPECT_STREQ((*candidates)[0].path, "/b");
  EXPECT_STREQ((*candidates)[1].path, "/c");
  EXPECT_STREQ((*candidates)[2].path, "/a");
  EXPECT_STREQ((*candidates)[3].path, "/z");
}
TEST_F(VirtualViewsTest, ScansVisibleBooksThroughTwoLevelsAndPreservesRecentMetadata) {
  fixture::add("/.hidden", true);
  fixture::add("/.hidden/book.epub");
  fixture::add("/TRASH", true);
  fixture::add("/TRASH/book.epub");
  fixture::add("/books", true);
  fixture::add("/books/second", true);
  fixture::add("/books/second/third", true);
  fixture::add("/books/second/third/too-deep.epub", false, date(2026, 9, 21));
  fixture::add("/books/second/new.epub", false, date(2026, 9, 21));
  fixture::add("/books/old.TXT", false, date(2026, 9, 20));
  fixture::add("/unknown.md");
  fixture::add("/cover.png");
  RECENT_BOOKS.books.push_back({"/books/second/new.epub", "Saved title", "Author", ""});
  std::vector<RecentBook> result;
  const auto status = VirtualViews::loadRecentlyAdded(result);
  ASSERT_EQ(result.size(), 3u);
  EXPECT_EQ(result[0].title, "Saved title");
  EXPECT_EQ(result[0].author, "Author");
  EXPECT_EQ(result[1].path, "/books/old.TXT");
  EXPECT_EQ(result[2].path, "/unknown.md");
  EXPECT_TRUE(status.unknownDates);
  EXPECT_FALSE(status.failed);
  EXPECT_FALSE(status.limited);
}
TEST_F(VirtualViewsTest, FinishedHistorySurvivesRemovalFromRecentsAndUsesCompletionDates) {
  fixture::add("/older.epub", false, date(2026, 9, 21));
  fixture::add("/newer.epub");
  fixture::add("/undated.epub");
  completed("/older.epub", {2026, 8, 1});
  completed("/newer.epub", {2026, 9, 1});
  completed("/undated.epub");
  std::vector<RecentBook> result;
  const auto status = VirtualViews::loadRecentlyFinished(result);
  ASSERT_EQ(result.size(), 3u);
  EXPECT_EQ(result[0].path, "/newer.epub");
  EXPECT_EQ(result[1].path, "/older.epub");
  EXPECT_EQ(result[2].path, "/undated.epub");
  EXPECT_TRUE(status.unknownDates);
  EXPECT_TRUE(RECENT_BOOKS.books.empty());
}
TEST_F(VirtualViewsTest, FinishedScanUsesValidatedExistingProgressOnly) {
  for (const auto* path : {"/near.epub", "/below.epub", "/corrupt.epub", "/missing.epub", "/xtc.xtc"})
    fixture::add(path);
  percent("/near.epub", 9950);
  percent("/below.epub", 9949);
  percent("/corrupt.epub", 10001);
  const auto xtcCache = "/.crosspoint/xtc_" + std::to_string(std::hash<std::string>{}("/xtc.xtc"));
  fixture::add(xtcCache, true);
  fixture::stats[xtcCache] = {true, {2026, 9, 20}};
  const auto before = fixture::files.size();
  std::vector<RecentBook> result;
  VirtualViews::loadRecentlyFinished(result);
  ASSERT_EQ(result.size(), 2u);
  EXPECT_EQ(result[0].path, "/xtc.xtc");
  EXPECT_EQ(result[1].path, "/near.epub");
  EXPECT_EQ(fixture::files.size(), before);
}
TEST_F(VirtualViewsTest, RejectsTruncatedAndWrongVersionProgress) {
  fixture::add("/truncated.epub");
  fixture::add("/version.epub");
  percent("/truncated.epub", 10000);
  percent("/version.epub", 10000);
  fixture::files[Epub::cachePathForFilePath("/truncated.epub", "/.crosspoint") + "/progress_percent.bin"]
      .data.pop_back();
  fixture::files[Epub::cachePathForFilePath("/version.epub", "/.crosspoint") + "/progress_percent.bin"].data[4] = 2;
  std::vector<RecentBook> result;
  VirtualViews::loadRecentlyFinished(result);
  EXPECT_TRUE(result.empty());
}
TEST_F(VirtualViewsTest, EntryAndTimeLimitsCloseAllHandles) {
  for (size_t i = 0; i < VirtualViews::MAX_SCAN_ENTRIES + 1; ++i) fixture::add("/" + std::to_string(i) + ".txt");
  std::vector<RecentBook> result;
  auto status = VirtualViews::loadRecentlyAdded(result);
  EXPECT_TRUE(status.limited);
  EXPECT_EQ(status.visited, VirtualViews::MAX_SCAN_ENTRIES);
  EXPECT_EQ(result.size(), 18u);
  fixture::tick = 500;
  status = VirtualViews::loadRecentlyAdded(result);
  EXPECT_TRUE(status.limited);
  EXPECT_LT(status.visited, 10u);
}
TEST_F(VirtualViewsTest, MissingRootAndDirectoryErrorsAreReported) {
  fixture::files.clear();
  std::vector<RecentBook> result;
  EXPECT_TRUE(VirtualViews::loadRecentlyAdded(result).failed);
  fixture::add("/", true);
  fixture::files["/"].failedIteration = true;
  EXPECT_TRUE(VirtualViews::loadRecentlyAdded(result).failed);
}
TEST_F(VirtualViewsTest, DirectoryWrapperAllocationFailureIsNotMistakenForCleanEof) {
  fixture::files["/"].failedAllocation = true;
  std::vector<RecentBook> result;
  EXPECT_TRUE(VirtualViews::loadRecentlyAdded(result).failed);
}
TEST_F(VirtualViewsTest, ZeroLimitClearsResultsWithoutScanning) {
  std::vector<RecentBook> result{{"old", "", "", ""}};
  EXPECT_EQ(VirtualViews::loadRecentlyAdded(result, 0).visited, 0u);
  EXPECT_TRUE(result.empty());
}
TEST_F(VirtualViewsTest, OlderHalWithoutTimestampsCompilesAndReturnsUnknown) {
  struct OlderHal {
  } file;
  uint16_t d = 0, t = 0;
  EXPECT_FALSE(VirtualViews::readModifyDateTime(file, &d, &t, 0));
  EXPECT_EQ(d, 0);
  EXPECT_EQ(t, 0);
}
}  // namespace
