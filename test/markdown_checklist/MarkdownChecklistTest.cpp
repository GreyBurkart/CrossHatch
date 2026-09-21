#include <HalStorage.h>
#include <gtest/gtest.h>
#include <util/MarkdownChecklist.h>

namespace {
using Status = MarkdownChecklist::Status;
constexpr const char* PATH = "/Preshow.md";
constexpr const char* TEMP = "/Preshow.md.checklist.tmp";
constexpr const char* BACKUP = "/Preshow.md.checklist.bak";
class MarkdownChecklistTest : public ::testing::Test {
 protected:
  void SetUp() override { fixture::reset(); }
  void TearDown() override {
    EXPECT_EQ(fixture::overlapping, 0u);
    for (const auto& [path, count] : fixture::handles) EXPECT_EQ(count, 0u) << path;
  }
};

TEST_F(MarkdownChecklistTest, ParsesHeadingsNestedBulletsOrderedTasksAndUppercaseChecks) {
  fixture::files[PATH] =
      "\xEF\xBB\xBF# Preshow ###\r\nNotes\r\n- [ ] Radios\r\n  * [x] Sound\r\n\t+ [X] Lamps\r\n1. [ ] Desk\r\n2) [ ] "
      "Finish";
  MarkdownChecklist document(PATH);
  ASSERT_EQ(document.load(), Status::Ok);
  EXPECT_EQ(document.rowCount(), 6u);
  EXPECT_EQ(document.taskCount(), 5u);
  EXPECT_EQ(document.completedCount(), 2u);
  EXPECT_TRUE(document.row(0).heading);
  EXPECT_STREQ(document.label(0), "Preshow");
  EXPECT_STREQ(document.label(2), "Sound");
  EXPECT_STREQ(document.label(5), "Finish");
}

TEST_F(MarkdownChecklistTest, IgnoresCodeFrontMatterCommentsAndMalformedTasks) {
  fixture::files[PATH] =
      "---\n- [ ] metadata\n---\n```md\n- [x] code\n```\n~~~~\n- [x] more code\n~~~~\n<!--\n- [ ] comment\n-->\n- [z] "
      "invalid\n- [x]no-space\n\\- [ ] escaped\n> - [ ] quote\n- [ ] Real";
  MarkdownChecklist document(PATH);
  ASSERT_EQ(document.load(), Status::Ok);
  ASSERT_EQ(document.taskCount(), 1u);
  EXPECT_STREQ(document.label(0), "Real");
}

TEST_F(MarkdownChecklistTest, PlainMarkdownAndEmptyFilesRemainTextDocuments) {
  for (const std::string content : {"", "# Notes\nJust some text\n", "```\n- [ ] example\n```"}) {
    fixture::files[PATH] = content;
    MarkdownChecklist document(PATH);
    EXPECT_EQ(document.load(), Status::NoTasks);
    EXPECT_EQ(fixture::files[PATH], content);
  }
}

TEST_F(MarkdownChecklistTest, SavesOnlyMarkerBytesPreservingUnicodeBomCrLfAndMissingFinalNewline) {
  const std::string original =
      "\xEF\xBB\xBF# Prép\r\nKeep **formatting**, links, and notes.\r\n- [ ] Rádios\r\n* [X] Sound";
  fixture::files[PATH] = original;
  MarkdownChecklist document(PATH);
  ASSERT_EQ(document.load(), Status::Ok);
  ASSERT_EQ(document.toggle(1), Status::Ok);
  std::string expected = original;
  expected[expected.find("[ ]") + 1] = 'x';
  EXPECT_EQ(fixture::files[PATH], expected);
  EXPECT_EQ(document.completedCount(), 2u);
  ASSERT_EQ(document.reset(), Status::Ok);
  expected[expected.find("[x]") + 1] = ' ';
  expected[expected.find("[X]") + 1] = ' ';
  EXPECT_EQ(fixture::files[PATH], expected);
  MarkdownChecklist reopened(PATH);
  EXPECT_EQ(reopened.load(), Status::Ok);
  EXPECT_EQ(reopened.completedCount(), 0u);
  EXPECT_FALSE(Storage.exists(TEMP));
  EXPECT_FALSE(Storage.exists(BACKUP));
}

TEST_F(MarkdownChecklistTest, HandlesMarkersAcrossStreamingBufferBoundaries) {
  fixture::files[PATH] = std::string(251, 'a') + "\n- [ ] Boundary\n- [x] Tail\n";
  MarkdownChecklist document(PATH);
  ASSERT_EQ(document.load(), Status::Ok);
  ASSERT_EQ(document.toggle(0), Status::Ok);
  EXPECT_NE(fixture::files[PATH].find("[x] Boundary"), std::string::npos);
  ASSERT_EQ(document.reset(), Status::Ok);
  EXPECT_EQ(document.completedCount(), 0u);
}

TEST_F(MarkdownChecklistTest, RefusesExternalEditsIncludingSameSizeChanges) {
  for (const std::string replacement : {"- [ ] New", "- [ ] Longer"}) {
    fixture::files[PATH] = "- [ ] Old";
    MarkdownChecklist document(PATH);
    ASSERT_EQ(document.load(), Status::Ok);
    fixture::files[PATH] = replacement;
    EXPECT_EQ(document.toggle(0), Status::SourceChanged);
    EXPECT_EQ(fixture::files[PATH], replacement);
    EXPECT_EQ(document.completedCount(), 0u);
    EXPECT_FALSE(Storage.exists(TEMP));
  }
}

TEST_F(MarkdownChecklistTest, RejectsOversizedFilesRowsLabelsAndLongTaskLinesWithoutWriting) {
  const std::string inputs[] = {
      std::string(MarkdownChecklist::MAX_FILE_BYTES + 1, 'a'),
      std::string("- [ ] ") + std::string(300, 'a'),
      [] {
        std::string s;
        for (unsigned i = 0; i < 129; ++i) s += "- [ ] Task\n";
        return s;
      }(),
      [] {
        std::string s;
        for (unsigned i = 0; i < 100; ++i) s += "- [ ] " + std::string(100, 'a') + "\n";
        return s;
      }(),
  };
  for (const auto& content : inputs) {
    fixture::files[PATH] = content;
    MarkdownChecklist document(PATH);
    EXPECT_EQ(document.load(), Status::LimitExceeded);
    EXPECT_EQ(document.toggle(0), Status::SaveError);
    EXPECT_EQ(fixture::files[PATH], content);
  }
}

TEST_F(MarkdownChecklistTest, ReadWriteSyncAndCloseFailuresKeepOriginalAndDisplayedState) {
  bool* flags[] = {&fixture::failRead, &fixture::failWrite, &fixture::failSync, &fixture::failClose,
                   &fixture::failCreate};
  for (bool* flag : flags) {
    fixture::files[PATH] = "- [ ] Keep me";
    MarkdownChecklist document(PATH);
    ASSERT_EQ(document.load(), Status::Ok);
    *flag = true;
    EXPECT_EQ(document.toggle(0), Status::SaveError);
    *flag = false;
    EXPECT_EQ(fixture::files[PATH], "- [ ] Keep me");
    EXPECT_EQ(document.completedCount(), 0u);
    EXPECT_FALSE(Storage.exists(TEMP));
    EXPECT_FALSE(Storage.exists(BACKUP));
    EXPECT_EQ(document.toggle(0), Status::Ok);
  }
}

TEST_F(MarkdownChecklistTest, RenameFailuresRollBackAndFailedRollbackKeepsRecoverableOriginal) {
  for (const std::set<unsigned>& failures : {std::set<unsigned>{1}, {2}, {2, 3}}) {
    fixture::reset();
    fixture::files[PATH] = "- [X] Keep me";
    MarkdownChecklist document(PATH);
    ASSERT_EQ(document.load(), Status::Ok);
    fixture::failRenames = failures;
    const bool rollbackFails = failures.count(3);
    EXPECT_EQ(document.reset(), rollbackFails ? Status::RecoveryNeeded : Status::SaveError);
    EXPECT_EQ(document.completedCount(), 1u);
    EXPECT_EQ(fixture::files[rollbackFails ? BACKUP : PATH], "- [X] Keep me");
    fixture::failRenames.clear();
    MarkdownChecklist reopened(PATH);
    EXPECT_EQ(reopened.load(), Status::Ok);
    EXPECT_EQ(reopened.completedCount(), 1u);
    EXPECT_EQ(fixture::files[PATH], "- [X] Keep me");
    EXPECT_FALSE(Storage.exists(TEMP));
  }
}

TEST_F(MarkdownChecklistTest, RecoversInterruptedSaveAtEachRenameStage) {
  for (bool installed : {false, true}) {
    fixture::reset();
    fixture::files[BACKUP] = "- [ ] Task";
    fixture::files[installed ? PATH : TEMP] = "- [x] Task";
    MarkdownChecklist document(PATH);
    EXPECT_EQ(document.load(), Status::Ok);
    EXPECT_EQ(document.completedCount(), installed ? 1u : 0u);
    EXPECT_FALSE(Storage.exists(TEMP));
    EXPECT_FALSE(Storage.exists(BACKUP));
  }
}

TEST_F(MarkdownChecklistTest, UncheckedResetDoesNotRewriteAndHeadingCannotBeToggled) {
  fixture::files[PATH] = "# Prep\n- [ ] Task";
  MarkdownChecklist document(PATH);
  ASSERT_EQ(document.load(), Status::Ok);
  EXPECT_EQ(document.toggle(0), Status::SaveError);
  EXPECT_EQ(document.reset(), Status::Ok);
  EXPECT_EQ(fixture::renameCount, 0u);
}
}  // namespace
