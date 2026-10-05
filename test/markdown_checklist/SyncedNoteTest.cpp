#include <HalStorage.h>
#include <gtest/gtest.h>
#include <util/MarkdownChecklist.h>
#include <util/SyncedNote.h>

namespace {
class SyncedNoteTest : public ::testing::Test {
 protected:
  void SetUp() override { fixture::reset(); }
};

TEST_F(SyncedNoteTest, OnlyTheFixedDocumentIsExternallyOwned) {
  EXPECT_TRUE(SyncedNote::isPath("/CrossHatch/Notes/TheList.md"));
  EXPECT_FALSE(SyncedNote::isPath("/Books/TheList.md"));
  EXPECT_FALSE(SyncedNote::isPath("/CrossHatch/Notes/Other.md"));
}

TEST_F(SyncedNoteTest, PublishesFirstUploadAndReplacesLocalChecklistEdits) {
  using namespace SyncedNote;
  ASSERT_TRUE(prepareUpload());
  fixture::files[TEMP_PATH] = "# TheList\nRemember to call John.\n- [ ] Update booth Mac\n";
  ASSERT_TRUE(finishUpload());
  MarkdownChecklist checklist(PATH);
  ASSERT_EQ(checklist.load(), MarkdownChecklist::Status::Ok);
  ASSERT_EQ(checklist.toggle(1), MarkdownChecklist::Status::Ok);
  EXPECT_NE(fixture::files[PATH].find("[x]"), std::string::npos);
  ASSERT_TRUE(prepareUpload());
  fixture::files[TEMP_PATH] = "# TheList\nHouse opens at 6:30.\n- [ ] Update booth Mac\n- [x] RF batteries\n";
  ASSERT_TRUE(finishUpload());
  ASSERT_EQ(checklist.load(), MarkdownChecklist::Status::Ok);
  EXPECT_EQ(checklist.completedCount(), 1u);
  EXPECT_FALSE(checklist.row(1).checked());
  EXPECT_NE(fixture::files[PATH].find("House opens at 6:30."), std::string::npos);
  EXPECT_FALSE(Storage.exists(TEMP_PATH));
  EXPECT_FALSE(Storage.exists(BACKUP_PATH));
  EXPECT_EQ(fixture::overlapping, 0u);
}

TEST_F(SyncedNoteTest, InterruptedTransferKeepsPreviousDocument) {
  using namespace SyncedNote;
  fixture::files[PATH] = "old";
  fixture::files[TEMP_PATH] = "partial";
  ASSERT_TRUE(recover());
  EXPECT_EQ(fixture::files[PATH], "old");
  EXPECT_FALSE(Storage.exists(TEMP_PATH));
}

TEST_F(SyncedNoteTest, FailedReplacementRestoresOriginal) {
  using namespace SyncedNote;
  for (unsigned failure : {1u, 2u}) {
    fixture::reset();
    fixture::files[PATH] = "old";
    fixture::files[TEMP_PATH] = "new";
    fixture::failRenames = {failure};
    EXPECT_FALSE(finishUpload());
    discardUpload();
    EXPECT_EQ(fixture::files[PATH], "old");
  }
}

TEST_F(SyncedNoteTest, FailedRollbackAndPowerLossRecoverFromBackup) {
  using namespace SyncedNote;
  fixture::files[PATH] = "old";
  fixture::files[TEMP_PATH] = "new";
  fixture::failRenames = {2, 3};
  EXPECT_FALSE(finishUpload());
  EXPECT_EQ(fixture::files[BACKUP_PATH], "old");
  fixture::failRenames.clear();
  ASSERT_TRUE(recover());
  EXPECT_EQ(fixture::files[PATH], "old");
  EXPECT_FALSE(Storage.exists(TEMP_PATH));
}

TEST_F(SyncedNoteTest, PublishedNoteWinsOverBackupAfterPowerLoss) {
  using namespace SyncedNote;
  fixture::files[PATH] = "new";
  fixture::files[BACKUP_PATH] = "old";
  ASSERT_TRUE(recover());
  EXPECT_EQ(fixture::files[PATH], "new");
  EXPECT_FALSE(Storage.exists(BACKUP_PATH));
}

TEST_F(SyncedNoteTest, StorageFailuresPreventStartingAnOverwrite) {
  using namespace SyncedNote;
  fixture::failMkdir = true;
  EXPECT_FALSE(prepareUpload());
  fixture::failMkdir = false;
  fixture::files[PATH] = "old";
  fixture::files[TEMP_PATH] = "partial";
  fixture::failRemove = true;
  EXPECT_FALSE(prepareUpload());
  EXPECT_EQ(fixture::files[PATH], "old");
}
}  // namespace
