#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

inline constexpr size_t CLIPPING_CHAPTER_TITLE_MAX = 48;
// Clipping text lives on the SD card rather than in every in-memory clipping
// record. Match the reader's bounded selection-text budget so previews retain
// a complete multi-paragraph selection without growing the saved-item index.
inline constexpr size_t CLIPPING_TEXT_MAX = 4U * 1024U;
inline constexpr uint16_t CLIPPING_MAX_PER_BOOK = 256;
inline constexpr uint16_t CLIPPING_MAX_PAGE_MATCHES = 16;
inline constexpr uint32_t CLIPPING_WORD_LAYOUT_VERSION = 2;
inline constexpr uint8_t CLIPPING_LAYOUT_START_RESOLVED = 1U << 0;
inline constexpr uint8_t CLIPPING_LAYOUT_END_RESOLVED = 1U << 1;
inline constexpr uint8_t CLIPPING_LAYOUT_BOUNDARIES_RESOLVED =
    CLIPPING_LAYOUT_START_RESOLVED | CLIPPING_LAYOUT_END_RESOLVED;

inline uint32_t clippingWordLayoutSignature(const uint32_t readerLayoutSignature) {
  if (readerLayoutSignature == 0) return 0;
  uint32_t signature = readerLayoutSignature;
  signature ^= CLIPPING_WORD_LAYOUT_VERSION;
  signature *= 16777619U;
  return signature == 0 ? 1 : signature;
}

struct Clipping {
  uint16_t spineIndex = 0;
  uint16_t startPage = 0;
  uint16_t endPage = 0;
  uint16_t pageCount = 1;
  uint16_t startWordIndex = 0;
  uint16_t endWordIndex = 0;
  uint16_t wordCount = 0;
  uint16_t paragraphIndex = UINT16_MAX;
  uint32_t timestamp = 0;
  uint32_t layoutSignature = 0;
  uint32_t textOffset = 0;
  uint16_t textLength = 0;
  uint16_t tableSelection = UINT16_MAX;
  // Session-only migration state, intentionally omitted from the on-disk
  // record.
  uint8_t resolvedLayoutBoundaries = 0;
  char chapterTitle[CLIPPING_CHAPTER_TITLE_MAX] = {};
  // PDF mutations use transactional rewrites and therefore retain their
  // bounded text while the PDF is open. EPUB text remains SD-backed.
  std::string text;
};

struct ClippedBookEntry {
  std::string bookTitle;
  std::string bookAuthor;
  std::string bookPath;
  std::string bookType;
  uint16_t count = 0;
};

class ClippingStore {
 public:
  enum class AddResult : uint8_t {
    Added,
    LimitReached,
    SaveFailed,
  };

  static ClippingStore& getInstance() { return instance; }

  bool loadForBook(const std::string& filePath, const std::string& title, const std::string& author,
                   const std::string& bookType);
  // Cold ambiguity-recovery path. On failure the reader disables saved-item
  // mutations instead of reconciling PSIT against an untrusted resident view.
  bool reloadPdfFromDisk();
  void unload();

  AddResult addClipping(uint16_t spineIndex, uint16_t startPage, uint16_t endPage, uint16_t pageCount,
                        uint16_t startWordIndex, uint16_t endWordIndex, uint16_t wordCount, const char* chapterTitle,
                        uint16_t paragraphIndex, const std::string& text, uint16_t tableSelection,
                        uint32_t layoutSignature);
  AddResult addPdfClipping(uint16_t spineIndex, uint16_t startPage, uint16_t endPage, uint16_t pageCount,
                           uint16_t startWordIndex, uint16_t endWordIndex, uint16_t wordCount, const char* chapterTitle,
                           uint16_t itemId, const std::string& text);
  bool removePdfClipping(uint16_t itemId);
  bool clearPdfClippings();
  bool removeClippingAt(size_t index);
  bool saveToFile();
  void clearAll();

  bool hasClippings() const { return !clippings.empty(); }
  size_t clippingCount() const { return clippings.size(); }
  const Clipping* clippingAt(size_t index) const;
  const std::vector<Clipping>& getClippings() const { return clippings; }
  bool cacheResolvedLayoutRange(size_t index, uint16_t page, uint16_t startWord, uint16_t endWord,
                                uint32_t layoutSignature);
  bool readClippingText(size_t index, std::string& out) const;
  bool readClippingPreview(size_t index, std::string& out) const;
  bool readClippingText(const Clipping& clipping, std::string& out) const;

  static bool hasAnyClippings();
  static bool getAllClippedBooks(std::vector<ClippedBookEntry>& out);
  static bool deleteForFilePath(const std::string& filePath, const std::string& bookType);
  // Cold directory-delete replay path: filename generation uses a bounded
  // stack buffer and never materializes either input view as an owning string.
  static bool deleteLegacyForFilePathNoPathAlloc(std::string_view filePath, std::string_view bookType);
  static bool deletePdfForFilePathNoPathAlloc(std::string_view filePath);
  // Journaled copy/verification is PDF-only. Legacy formats keep using
  // migrateForFilePath after their source rename.
  static bool copyForFilePath(const std::string& oldFilePath, const std::string& newFilePath,
                              const std::string& bookType);
  static bool verifyCopyForFilePath(const std::string& oldFilePath, const std::string& newFilePath,
                                    const std::string& bookType);
  static bool migrateForFilePath(const std::string& oldFilePath, const std::string& newFilePath,
                                 const std::string& title, const std::string& author, const std::string& bookType);

 private:
  static ClippingStore instance;

  std::vector<Clipping> clippings;
  std::string bookFilePath;
  std::string bookTitle;
  std::string bookAuthor;
  std::string storeFilePath;
  bool dirty = false;

  bool readFromFile();
  bool readFromFile(const std::string& path, std::vector<Clipping>& out) const;
  bool writeToFile(const std::string* replacementText = nullptr, size_t replacementIndex = SIZE_MAX);
  bool writeMigrationPayload(void* fileContext) const;
  bool writePdfTransaction(const Clipping* appended, size_t removeIndex, bool clear) const;
  bool verifyPdfTransaction(const std::string& path, const Clipping* appended, size_t removeIndex, bool clear) const;
  bool recoverPdfTransaction() const;
  static bool migratePdfForFilePath(const std::string& oldFilePath, const std::string& newFilePath,
                                    const std::string& title, const std::string& author);
  static bool migrateLegacyForFilePath(const std::string& oldFilePath, const std::string& newFilePath,
                                       const std::string& title, const std::string& author,
                                       const std::string& bookType);
};

inline bool clippingStoredRangeMatchesLayout(const Clipping& clipping, const uint16_t currentPageCount,
                                             const uint32_t currentLayoutSignature) {
  if (clipping.pageCount != currentPageCount) return false;
  // Legacy records have no signature, so their body-only word ordinals cannot
  // be trusted now that table words participate in clipping. Their saved text
  // is used to find the highlight instead.
  return clipping.layoutSignature != 0 && currentLayoutSignature != 0 &&
         clipping.layoutSignature == currentLayoutSignature;
}

inline bool clippingUsesLegacyWordLayout(const Clipping& clipping, const uint16_t currentPageCount,
                                         const uint32_t readerLayoutSignature) {
  if (clipping.pageCount != currentPageCount || clipping.layoutSignature == 0 || readerLayoutSignature == 0) {
    return false;
  }
  return clipping.layoutSignature == readerLayoutSignature &&
         clipping.layoutSignature != clippingWordLayoutSignature(readerLayoutSignature);
}

inline bool clippingCachedRangeReadyOnPage(const Clipping& clipping, const uint16_t page) {
  if (clipping.startPage > clipping.endPage || page < clipping.startPage || page > clipping.endPage) {
    return false;
  }
  if (page == clipping.startPage && (clipping.resolvedLayoutBoundaries & CLIPPING_LAYOUT_START_RESOLVED) == 0) {
    return false;
  }
  if (page == clipping.endPage && (clipping.resolvedLayoutBoundaries & CLIPPING_LAYOUT_END_RESOLVED) == 0) {
    return false;
  }
  return true;
}

inline bool cacheClippingResolvedLayoutRange(Clipping& clipping, const uint16_t page, const uint16_t startWord,
                                             const uint16_t endWord, const uint32_t layoutSignature) {
  if (layoutSignature == 0 || clipping.startPage > clipping.endPage || page < clipping.startPage ||
      page > clipping.endPage || startWord > endWord) {
    return false;
  }

  bool changed = false;
  if (page == clipping.startPage) {
    changed = clipping.startWordIndex != startWord ||
              (clipping.resolvedLayoutBoundaries & CLIPPING_LAYOUT_START_RESOLVED) == 0;
    clipping.startWordIndex = startWord;
    clipping.resolvedLayoutBoundaries |= CLIPPING_LAYOUT_START_RESOLVED;
  }
  if (page == clipping.endPage) {
    changed = changed || clipping.endWordIndex != endWord ||
              (clipping.resolvedLayoutBoundaries & CLIPPING_LAYOUT_END_RESOLVED) == 0;
    clipping.endWordIndex = endWord;
    clipping.resolvedLayoutBoundaries |= CLIPPING_LAYOUT_END_RESOLVED;
  }
  if ((clipping.resolvedLayoutBoundaries & CLIPPING_LAYOUT_BOUNDARIES_RESOLVED) ==
          CLIPPING_LAYOUT_BOUNDARIES_RESOLVED &&
      clipping.layoutSignature != layoutSignature) {
    clipping.layoutSignature = layoutSignature;
    changed = true;
  }
  return changed;
}

#define CLIPPINGS ClippingStore::getInstance()
