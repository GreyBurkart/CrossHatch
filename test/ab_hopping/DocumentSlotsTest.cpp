#include <gtest/gtest.h>

#include "util/DocumentSlots.h"

TEST(DocumentSlots, SwitchesBetweenAssignedDocumentsWithoutChangingAssignments) {
  DocumentSlots slots;
  ASSERT_TRUE(slots.assignDocumentSlot("/primary.epub", 0));
  ASSERT_TRUE(slots.assignDocumentSlot("/notes.txt", 1));
  EXPECT_EQ(slots.getTargetHopPath("/primary.epub"), "/notes.txt");
  EXPECT_EQ(slots.getTargetHopPath("/notes.txt"), "/primary.epub");
  EXPECT_EQ(slots.activeDocSlot, 0);
  slots.noteOpenedDocument("/notes.txt");
  EXPECT_EQ(slots.activeDocSlot, 1);
}

TEST(DocumentSlots, UnrelatedDocumentReturnsToLastSlotWithoutOverwritingIt) {
  DocumentSlots slots;
  ASSERT_TRUE(slots.assignDocumentSlot("/primary.epub", 0));
  ASSERT_TRUE(slots.assignDocumentSlot("/reference.xtc", 1));
  slots.noteOpenedDocument("/reference.xtc");
  slots.noteOpenedDocument("/third.epub");
  EXPECT_EQ(slots.getTargetHopPath("/third.epub"), "/reference.xtc");
  EXPECT_EQ(slots.docSlotA, "/primary.epub");
  EXPECT_EQ(slots.docSlotB, "/reference.xtc");
  EXPECT_EQ(slots.activeDocSlot, 1);
}

TEST(DocumentSlots, UnassignedDestinationIsEmptyAndNeverReturnsCurrentDocument) {
  DocumentSlots slots;
  EXPECT_TRUE(slots.getTargetHopPath("/third.epub").empty());
  ASSERT_TRUE(slots.assignDocumentSlot("/primary.epub", 0));
  EXPECT_TRUE(slots.getTargetHopPath("/primary.epub").empty());
  EXPECT_EQ(slots.getTargetHopPath("/third.epub"), "/primary.epub");
  DocumentSlots onlyB;
  ASSERT_TRUE(onlyB.assignDocumentSlot("/notes.txt", 1));
  EXPECT_TRUE(onlyB.getTargetHopPath("/notes.txt").empty());
  EXPECT_EQ(onlyB.getTargetHopPath(""), "/notes.txt");
}

TEST(DocumentSlots, DuplicateAssignmentMovesTheDocumentIncludingAliasedInput) {
  DocumentSlots slots;
  ASSERT_TRUE(slots.assignDocumentSlot("/primary.epub", 0));
  ASSERT_TRUE(slots.assignDocumentSlot(slots.docSlotA, 1));
  EXPECT_TRUE(slots.docSlotA.empty());
  EXPECT_EQ(slots.docSlotB, "/primary.epub");
  EXPECT_TRUE(slots.getTargetHopPath("/primary.epub").empty());
}

TEST(DocumentSlots, RejectsUnsupportedInvalidAndOversizedPathsWithoutChangingSlots) {
  DocumentSlots slots;
  ASSERT_TRUE(slots.assignDocumentSlot("/primary.epub", 0));
  for (const char* path :
       {"", "relative.epub", "/", "/file.pdf", "/folder/../file.txt", "/folder/./file.txt", "/folder//file.txt"}) {
    EXPECT_FALSE(slots.assignDocumentSlot(path, 0)) << path;
  }
  EXPECT_FALSE(slots.assignDocumentSlot("/valid.txt", 2));
  EXPECT_FALSE(slots.assignDocumentSlot("/" + std::string(513, 'x') + ".epub", 0));
  EXPECT_FALSE(slots.assignDocumentSlot(std::string("/book\0.epub", 11), 0));
  EXPECT_EQ(slots.docSlotA, "/primary.epub");
  EXPECT_TRUE(slots.docSlotB.empty());
}

TEST(DocumentSlots, AcceptsSupportedCaseInsensitiveExtensions) {
  for (const char* path : {"/Book.EPUB", "/Notes.TXT", "/Book.XTC", "/Book.XTCH", "/Notes.MD"}) {
    EXPECT_TRUE(DocumentSlots::validDocumentPath(path)) << path;
  }
}

TEST(DocumentSlots, JsonRoundTripPreservesPathsAndLastOpenedSlot) {
  DocumentSlots slots;
  ASSERT_TRUE(slots.assignDocumentSlot("/books/A.epub", 0));
  ASSERT_TRUE(slots.assignDocumentSlot("/notes/B.txt", 1));
  slots.noteOpenedDocument("/notes/B.txt");
  JsonDocument doc;
  slots.documentSlotsToJson(doc);
  std::string serialized;
  serializeJson(doc, serialized);
  JsonDocument parsed;
  ASSERT_EQ(deserializeJson(parsed, serialized), DeserializationError::Ok);
  DocumentSlots restored;
  restored.documentSlotsFromJson(parsed.as<JsonVariantConst>());
  EXPECT_EQ(restored.docSlotA, slots.docSlotA);
  EXPECT_EQ(restored.docSlotB, slots.docSlotB);
  EXPECT_EQ(restored.activeDocSlot, 1);
  EXPECT_EQ(restored.getTargetHopPath("/notes/B.txt"), "/books/A.epub");
}

TEST(DocumentSlots, LegacyStateWithoutSlotsClearsAnyStaleSessionAssignments) {
  DocumentSlots slots;
  ASSERT_TRUE(slots.assignDocumentSlot("/old.epub", 0));
  ASSERT_TRUE(slots.assignDocumentSlot("/old.txt", 1));
  slots.activeDocSlot = 1;
  JsonDocument legacy;
  legacy["openEpubPath"] = "/book.epub";
  slots.documentSlotsFromJson(legacy.as<JsonVariantConst>());
  EXPECT_TRUE(slots.docSlotA.empty());
  EXPECT_TRUE(slots.docSlotB.empty());
  EXPECT_EQ(slots.activeDocSlot, 0);
}

TEST(DocumentSlots, CorruptPersistedDataCannotProduceDuplicateOrInvalidSlots) {
  JsonDocument doc;
  doc["docSlotA"] = "/same.epub";
  doc["docSlotB"] = "/same.epub";
  doc["activeDocSlot"] = 255;
  DocumentSlots slots;
  slots.documentSlotsFromJson(doc.as<JsonVariantConst>());
  EXPECT_EQ(slots.docSlotA, "/same.epub");
  EXPECT_TRUE(slots.docSlotB.empty());
  EXPECT_EQ(slots.activeDocSlot, 0);

  doc["docSlotA"] = 7;
  doc["docSlotB"] = "/only.txt";
  doc["activeDocSlot"] = -1;
  slots.documentSlotsFromJson(doc.as<JsonVariantConst>());
  EXPECT_TRUE(slots.docSlotA.empty());
  EXPECT_EQ(slots.docSlotB, "/only.txt");
  EXPECT_EQ(slots.activeDocSlot, 1);

  doc["docSlotB"] = "/" + std::string(513, 'x') + ".epub";
  slots.documentSlotsFromJson(doc.as<JsonVariantConst>());
  EXPECT_TRUE(slots.docSlotA.empty());
  EXPECT_TRUE(slots.docSlotB.empty());
}
