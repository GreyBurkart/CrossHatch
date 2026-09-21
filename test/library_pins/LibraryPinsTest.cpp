#include <CrossPointSettings.h>
#include <LibraryPins.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>

namespace {
constexpr uint8_t docAction = CrossPointSettings::OPEN_PINNED_DOC;
constexpr uint8_t folderAction = CrossPointSettings::OPEN_PINNED_FOLDER;

void fillSlots(CrossPointSettings& settings, const std::array<uint8_t, 5>& slots) {
  std::copy(slots.begin(), slots.end(), settings.quickActionSlots);
}

std::array<uint8_t, 5> slots(const CrossPointSettings& settings) {
  std::array<uint8_t, 5> result;
  std::copy(std::begin(settings.quickActionSlots), std::end(settings.quickActionSlots), result.begin());
  return result;
}
}  // namespace

TEST(LibraryPins, PinsDocumentInFirstFreeSlotAndKeepsFolder) {
  CrossPointSettings settings;
  settings.pinnedFolderPath = "/Technical";
  fillSlots(settings, {1, 0, folderAction, 0, 7});
  ASSERT_TRUE(LibraryPins::set(settings, "/Books/Manual.epub", false, true));
  EXPECT_EQ(settings.pinnedDocPath, "/Books/Manual.epub");
  EXPECT_EQ(settings.pinnedFolderPath, "/Technical");
  EXPECT_EQ(slots(settings), (std::array<uint8_t, 5>{1, docAction, folderAction, 0, 7}));
  EXPECT_EQ(settings.savedDocument, settings.pinnedDocPath);
  EXPECT_EQ(settings.savedSlots, slots(settings));
  EXPECT_EQ(settings.saveCalls, 1U);
}

TEST(LibraryPins, FullPopupStillSavesPinWithoutReplacingConfiguredActions) {
  CrossPointSettings settings;
  fillSlots(settings, {1, 2, 3, 4, 5});
  ASSERT_TRUE(LibraryPins::set(settings, "/Reference", true, true));
  EXPECT_EQ(settings.savedFolder, "/Reference");
  EXPECT_EQ(slots(settings), (std::array<uint8_t, 5>{1, 2, 3, 4, 5}));
  EXPECT_EQ(settings.savedSlots, slots(settings));
}

TEST(LibraryPins, ReplacingDocumentDoesNotAddDuplicatePopupAction) {
  CrossPointSettings settings;
  settings.pinnedDocPath = "/Old.epub";
  fillSlots(settings, {0, 3, docAction, 0, 4});
  ASSERT_TRUE(LibraryPins::set(settings, "/New.md", false, true));
  EXPECT_EQ(settings.savedDocument, "/New.md");
  EXPECT_EQ(slots(settings), (std::array<uint8_t, 5>{0, 3, docAction, 0, 4}));
}

TEST(LibraryPins, FailedPinWriteRestoresPathAndPopupSlots) {
  CrossPointSettings settings;
  settings.pinnedDocPath = "/Existing.epub";
  settings.pinnedFolderPath = "/Books";
  fillSlots(settings, {1, 0, folderAction, 4, 0});
  ASSERT_TRUE(settings.saveToFile());
  settings.failSave = true;
  EXPECT_FALSE(LibraryPins::set(settings, "/Replacement.txt", false, true));
  EXPECT_EQ(settings.pinnedDocPath, settings.savedDocument);
  EXPECT_EQ(settings.pinnedFolderPath, settings.savedFolder);
  EXPECT_EQ(slots(settings), settings.savedSlots);
}

TEST(LibraryPins, UnpinClearsMissingTargetAndEveryMatchingSlot) {
  CrossPointSettings settings;
  settings.pinnedDocPath = "/NowMissing.epub";
  settings.pinnedFolderPath = "/Library";
  fillSlots(settings, {docAction, folderAction, 3, docAction, 4});
  ASSERT_TRUE(LibraryPins::set(settings, "/NowMissing.epub", false, false));
  EXPECT_TRUE(settings.pinnedDocPath.empty());
  EXPECT_EQ(settings.pinnedFolderPath, "/Library");
  EXPECT_EQ(slots(settings), (std::array<uint8_t, 5>{0, folderAction, 3, 0, 4}));
  EXPECT_TRUE(settings.savedDocument.empty());
}

TEST(LibraryPins, FailedUnpinRestoresFolderAndPopup) {
  CrossPointSettings settings;
  settings.pinnedFolderPath = "/KeepThis";
  fillSlots(settings, {1, folderAction, docAction, 0, 0});
  ASSERT_TRUE(settings.saveToFile());
  settings.failSave = true;
  EXPECT_FALSE(LibraryPins::set(settings, "/KeepThis", true, false));
  EXPECT_EQ(settings.pinnedFolderPath, settings.savedFolder);
  EXPECT_EQ(slots(settings), settings.savedSlots);
}

TEST(LibraryPins, StaleUnpinCannotEraseAReplacementPin) {
  CrossPointSettings settings;
  settings.pinnedDocPath = "/Replacement.epub";
  fillSlots(settings, {docAction, 0, 0, 0, 0});
  ASSERT_TRUE(LibraryPins::set(settings, "/Previous.epub", false, false));
  EXPECT_EQ(settings.pinnedDocPath, "/Replacement.epub");
  EXPECT_EQ(settings.quickActionSlots[0], docAction);
  EXPECT_EQ(settings.saveCalls, 0U);
}

TEST(LibraryPins, MaximumLengthPathIsSavedWhole) {
  CrossPointSettings settings;
  const std::string path = "/" + std::string(LibraryPins::MAX_PATH_LENGTH - 6, 'a') + ".epub";
  ASSERT_EQ(path.size(), 1023U);
  ASSERT_TRUE(LibraryPins::set(settings, path, false, true));
  EXPECT_EQ(settings.pinnedDocPath, path);
  EXPECT_EQ(settings.savedDocument, path);
}

TEST(LibraryPins, InvalidPathsDoNotReplaceAnExistingPin) {
  CrossPointSettings settings;
  settings.pinnedDocPath = "/Keep.epub";
  fillSlots(settings, {docAction, 0, 0, 0, 0});
  for (const auto& path : {std::string(), std::string("relative.epub"), "/" + std::string(1023, 'a')}) {
    EXPECT_FALSE(LibraryPins::set(settings, path, false, true));
    EXPECT_EQ(settings.pinnedDocPath, "/Keep.epub");
    EXPECT_EQ(settings.quickActionSlots[0], docAction);
    EXPECT_EQ(settings.saveCalls, 0U);
  }
}

TEST(LibraryPins, RejectsTraversalNulAndAmbiguousSeparators) {
  CrossPointSettings settings;
  settings.pinnedFolderPath = "/Keep";
  const std::array<std::string, 9> invalid = {"/.",
                                              "/..",
                                              "/Books/../Other",
                                              "/Books/./Manual",
                                              "//Books",
                                              "/Books//Manual",
                                              "/Books/",
                                              "//",
                                              std::string("/Books\0/Other", 13)};
  for (const auto& path : invalid) {
    EXPECT_FALSE(LibraryPins::validPath(path));
    EXPECT_FALSE(LibraryPins::set(settings, path, true, true));
    EXPECT_EQ(settings.pinnedFolderPath, "/Keep");
    EXPECT_EQ(settings.saveCalls, 0U);
  }
}

TEST(LibraryPins, AcceptsRootHiddenNamesAndUtf8WithoutChangingThem) {
  CrossPointSettings settings;
  for (const auto* path : {"/", "/.reference", "/Books/..notes", "/Books/My résumé.epub"}) {
    EXPECT_TRUE(LibraryPins::validPath(path));
    ASSERT_TRUE(LibraryPins::set(settings, path, true, true));
    EXPECT_EQ(settings.pinnedFolderPath, path);
    EXPECT_EQ(settings.savedFolder, path);
  }
}
