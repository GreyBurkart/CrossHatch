#include <gtest/gtest.h>

#include <set>
#include <string>

#include "TrashPaths.h"

using namespace crosshatch::trash;

TEST(TrashPathsTest, PathDetection) {
  EXPECT_TRUE(isDirectory("/trash"));
  EXPECT_TRUE(isDirectory("/TRASH"));
  EXPECT_FALSE(isDirectory("/trash/book.epub"));
  EXPECT_FALSE(isDirectory("/books"));
  EXPECT_FALSE(isDirectory(nullptr));

  EXPECT_TRUE(isPath("/trash"));
  EXPECT_TRUE(isPath("/trash/book.epub"));
  EXPECT_TRUE(isPath("/trash/subfolder/book.epub"));
  EXPECT_FALSE(isPath("/trashcan"));
  EXPECT_FALSE(isPath("/books"));
  EXPECT_FALSE(isPath(nullptr));

  EXPECT_TRUE(containsPath("/trash", "/trash/book.epub"));
  EXPECT_TRUE(containsPath("/trash/", "/trash/book.epub"));
  EXPECT_TRUE(containsPath("/", "/trash/book.epub"));
  EXPECT_FALSE(containsPath("/trash", "/trashcan/book.epub"));
  EXPECT_FALSE(containsPath("/books", "/trash/book.epub"));
}

TEST(TrashPathsTest, BuildTrashParent) {
  char parent[256];
  EXPECT_TRUE(buildTrashParent(parent, sizeof(parent), "/book.epub"));
  EXPECT_STREQ(parent, "/trash");

  EXPECT_TRUE(buildTrashParent(parent, sizeof(parent), "/books/sci-fi/dune.epub"));
  EXPECT_STREQ(parent, "/trash/books/sci-fi");

  EXPECT_FALSE(buildTrashParent(parent, sizeof(parent), ""));
  EXPECT_FALSE(buildTrashParent(parent, sizeof(parent), "/"));
}

TEST(TrashPathsTest, BuildRestoreParent) {
  char parent[256];
  EXPECT_TRUE(buildRestoreParent(parent, sizeof(parent), "/trash/book.epub"));
  EXPECT_STREQ(parent, "/");

  EXPECT_TRUE(buildRestoreParent(parent, sizeof(parent), "/trash/books/sci-fi/dune.epub"));
  EXPECT_STREQ(parent, "/books/sci-fi");

  EXPECT_FALSE(buildRestoreParent(parent, sizeof(parent), "/trash"));
  EXPECT_FALSE(buildRestoreParent(parent, sizeof(parent), "/trash/"));
  EXPECT_FALSE(buildRestoreParent(parent, sizeof(parent), "/other/book.epub"));
}

TEST(TrashPathsTest, BuildCandidate) {
  char path[256];
  EXPECT_TRUE(buildCandidate(path, sizeof(path), "/trash", "Book.epub", 1));
  EXPECT_STREQ(path, "/trash/Book.epub");

  EXPECT_TRUE(buildCandidate(path, sizeof(path), "/trash", "Book.epub", 2));
  EXPECT_STREQ(path, "/trash/Book (2).epub");

  EXPECT_TRUE(buildCandidate(path, sizeof(path), "/trash", "README", 3));
  EXPECT_STREQ(path, "/trash/README (3)");

  EXPECT_TRUE(buildCandidate(path, sizeof(path), "/", "Book.epub", 1));
  EXPECT_STREQ(path, "/Book.epub");

  EXPECT_TRUE(buildCandidate(path, sizeof(path), "/trash/", "Book.epub", 1));
  EXPECT_STREQ(path, "/trash/Book.epub");

  char small[12];
  EXPECT_FALSE(buildCandidate(small, sizeof(small), "/trash", "Book.epub", 1));
}

TEST(TrashPathsTest, FindVacantPath) {
  char path[256];
  std::set<std::string> occupied{"/trash/Book.epub", "/trash/Book (2).epub"};
  const auto exists = [&occupied](const char* candidate) {
    return occupied.count(candidate) != 0 ? PathProbe::Occupied : PathProbe::Vacant;
  };

  EXPECT_TRUE(findVacantPath(path, sizeof(path), "/trash", "Book.epub", exists));
  EXPECT_STREQ(path, "/trash/Book (3).epub");

  unsigned probeCalls = 0;
  const auto failingProbe = [&probeCalls](const char*) {
    probeCalls++;
    return PathProbe::Failed;
  };
  EXPECT_FALSE(findVacantPath(path, sizeof(path), "/trash", "Book.epub", failingProbe));
  EXPECT_EQ(probeCalls, 1u);
}

TEST(TrashPathsTest, FindRestorePath) {
  char path[256];
  char restoreParent[64];
  std::set<std::string> occupied;
  const auto exists = [&occupied](const char* candidate) {
    return occupied.count(candidate) != 0 ? PathProbe::Occupied : PathProbe::Vacant;
  };

  const auto missingParent = [](const char*) { return false; };
  EXPECT_TRUE(findRestorePath(path, sizeof(path), restoreParent, sizeof(restoreParent), "/trash/books/Book.epub",
                              "Book.epub", missingParent, exists));
  EXPECT_STREQ(path, "/Book.epub");

  std::set<std::string> restoredOccupied{"/books/Book.epub", "/books/Book (2).epub"};
  const auto parentReady = [](const char* parent) { return std::string(parent) == "/books"; };
  const auto restoredExists = [&restoredOccupied](const char* candidate) {
    return restoredOccupied.count(candidate) != 0 ? PathProbe::Occupied : PathProbe::Vacant;
  };
  EXPECT_TRUE(findRestorePath(path, sizeof(path), restoreParent, sizeof(restoreParent), "/trash/books/Book.epub",
                              "Book.epub", parentReady, restoredExists));
  EXPECT_STREQ(path, "/books/Book (3).epub");
}

TEST(TrashPathsTest, DeleteActionRules) {
  EXPECT_EQ(deleteAction(false, false, true), DeleteAction::MoveToTrash);
  EXPECT_EQ(deleteAction(false, false, false), DeleteAction::PermanentlyDelete);
  EXPECT_EQ(deleteAction(false, true, true), DeleteAction::PermanentlyDelete);
  EXPECT_EQ(deleteAction(true, false, true), DeleteAction::PermanentlyDeleteDirectory);
  EXPECT_EQ(deleteAction(true, true, true), DeleteAction::PermanentlyDeleteDirectory);
}
