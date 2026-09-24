#include "Section.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <InflateStream.h>
#include <Logging.h>
#include <Memory.h>
#include <MemoryBudget.h>
#include <PdfCacheIo.h>
#include <PdfHalIo.h>
#include <PdfLayoutWordIndex.h>
#include <PdfWordCounter.h>
#include <Serialization.h>

#include <cstdio>
#include <cstring>

#include "Epub/css/CssParser.h"
#include "Page.h"
#include "SectionPageIndexSerialization.h"
#include "blocks/TextBlock.h"
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
constexpr uint32_t SECTION_CACHE_MAGIC = 0x535843FF;  // bytes: 0xFF, "CXS"
// v62: Text blocks persist source whitespace semantics for font-preview reflow.
// This changes their serialized payload, so full and suspended section caches
// must rebuild together.
// v63: Paragraph base direction excludes direction changes from inline elements.
// v66: Internal EPUB links preserve CSS superscript/subscript positioning.
constexpr uint8_t SECTION_FILE_VERSION = 66;
// Suspended incremental build: valid pages plus LUTs and a parse-watermark trailer.
// Change this with layout or payload changes so stale partial pages cannot resume
// under a different layout contract.
constexpr uint8_t SECTION_FILE_PARTIAL_VERSION = 0xF6;
constexpr uint32_t HEADER_SIZE =
    sizeof(SECTION_CACHE_MAGIC) + sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(bool) + sizeof(bool) +
    sizeof(uint8_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) + sizeof(bool) + sizeof(uint8_t) +
    sizeof(bool) + sizeof(bool) + sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint16_t) + sizeof(uint32_t) +
    sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);
constexpr size_t SECTION_HTML_STREAM_CHUNK_SIZE = 8192;
constexpr size_t LOW_MEMORY_SECTION_HTML_STREAM_CHUNK_SIZE = 1024;

void prepareSectionZipInflate(GfxRenderer& renderer, const int fontId) {
  if (ESP.getMaxAllocHeap() < InflateStream::requiredInternalStorageSize(true) && renderer.isSdCardFont(fontId)) {
    renderer.releaseSdCardFontForLowMemory(fontId);
  }
}

size_t sectionHtmlStreamChunkSize(const bool preview) {
  if (preview) {
    return LOW_MEMORY_SECTION_HTML_STREAM_CHUNK_SIZE;
  }

  const uint32_t maxAlloc = ESP.getMaxAllocHeap();
  const size_t largeStreamBudget =
      InflateStream::requiredInternalStorageSize(true) + (2U * SECTION_HTML_STREAM_CHUNK_SIZE);
  if (maxAlloc < largeStreamBudget) {
    LOG_DBG("SCT", "Using low-memory HTML stream chunk (maxAlloc=%u)", maxAlloc);
    return LOW_MEMORY_SECTION_HTML_STREAM_CHUNK_SIZE;
  }
  return SECTION_HTML_STREAM_CHUNK_SIZE;
}

PdfStatus patchLayoutWordIndex(void* const context, const uint64_t offset, const uint8_t* const source,
                               const size_t requested, size_t* const bytesWritten) {
  if (context == nullptr || source == nullptr || bytesWritten == nullptr || offset > SIZE_MAX) {
    return PdfStatus::failure(PdfError::InvalidArgument, offset);
  }
  auto& file = *static_cast<HalFile*>(context);
  *bytesWritten = 0;
  if (!file.seek(static_cast<size_t>(offset))) {
    return PdfStatus::failure(PdfError::IoFailure, offset);
  }
  *bytesWritten = file.write(source, requested);
  return PdfStatus::success();
}

std::string sectionBackupPath(const std::string& filePath) { return filePath + ".bak"; }

void recoverSectionCacheBackup(const std::string& filePath) {
  const std::string backupPath = sectionBackupPath(filePath);
  if (!Storage.exists(backupPath.c_str())) return;

  if (Storage.exists(filePath.c_str())) {
    Storage.remove(backupPath.c_str());
    return;
  }

  if (Storage.rename(backupPath.c_str(), filePath.c_str())) {
    LOG_INF("SCT", "Recovered section cache backup: %s", filePath.c_str());
  } else {
    LOG_ERR("SCT", "Failed to recover section cache backup: %s", filePath.c_str());
  }
}

bool promoteSectionCache(const std::string& tmpPath, const std::string& filePath) {
  recoverSectionCacheBackup(filePath);
  if (!Storage.exists(filePath.c_str())) {
    return Storage.rename(tmpPath.c_str(), filePath.c_str());
  }

  const std::string backupPath = sectionBackupPath(filePath);
  if (!Storage.rename(filePath.c_str(), backupPath.c_str())) {
    LOG_ERR("SCT", "Failed to preserve old section cache before replacement");
    return false;
  }
  if (Storage.rename(tmpPath.c_str(), filePath.c_str())) {
    Storage.remove(backupPath.c_str());
    return true;
  }

  LOG_ERR("SCT", "Failed to promote section cache; restoring previous cache");
  if (!Storage.rename(backupPath.c_str(), filePath.c_str())) {
    LOG_ERR("SCT", "Failed to restore section cache backup");
  }
  return false;
}
}  // namespace

struct Section::PdfPageBuildContext {
  Section& section;
  bool& pageCompletionFailed;
  // PDF pages share the EPUB page index; the sidecar only adds word ranges.
  SectionPageIndex& pageIndex;
  bool* layoutAbortedForLowMemory;
  HalFile sidecarFile;
  PdfLayoutWordIndexWriter writer;
  PdfWordCounter wordCounter;
  PdfLayoutWordRange pageRange{};
  uint32_t sectionFirstWordOrdinal = 0;
  uint32_t sectionWordCount = 0;
  uint32_t nextGlobalWordOrdinal = 0;
  uint32_t wordsExtractedInBlock = 0;
  size_t scannedPageElements = 0;
  PdfLayoutCacheBinding sectionBinding{};
  char currentBlockAnchor[PDF_LAYOUT_WORD_ANCHOR_BYTES] = {};
  char finalPath[PDF_CACHE_PATH_CAPACITY] = {};
  char tempPath[PDF_CACHE_PATH_CAPACITY] = {};
  bool blockHasToken = false;
  bool promoted = false;

  PdfLayoutWordIndexInfo replayInfo{};

  PdfPageBuildContext(Section& section, bool& pageCompletionFailed, SectionPageIndex& pageIndex,
                      bool* layoutAbortedForLowMemory)
      : section(section),
        pageCompletionFailed(pageCompletionFailed),
        pageIndex(pageIndex),
        layoutAbortedForLowMemory(layoutAbortedForLowMemory) {}

  ~PdfPageBuildContext() {
    if (sidecarFile) {
      sidecarFile.close();
    }
    if (!promoted && tempPath[0] != '\0' && Storage.exists(tempPath)) {
      Storage.remove(tempPath);
    }
  }

  bool initialize(const std::string& sectionCachePath, const int sectionIndex, const ReflowSectionInfo& info) {
    const int finalLength = std::snprintf(finalPath, sizeof(finalPath), "%s.pwi", sectionCachePath.c_str());
    const int tempLength = finalLength > 0 && static_cast<size_t>(finalLength) < sizeof(finalPath)
                               ? std::snprintf(tempPath, sizeof(tempPath), "%s.tmp", finalPath)
                               : -1;
    if (tempLength <= 0 || static_cast<size_t>(tempLength) >= sizeof(tempPath)) {
      LOG_ERR("SCT", "PDF semantic sidecar path exceeds fixed capacity");
      return false;
    }
    if (Storage.exists(tempPath)) {
      Storage.remove(tempPath);
    }
    if (!Storage.openFileForWrite("SCT", tempPath, sidecarFile)) {
      LOG_ERR("SCT", "Failed to create PDF semantic sidecar");
      return false;
    }
    const PdfStatus status = writer.begin(pdfHalByteSink(sidecarFile), static_cast<uint16_t>(sectionIndex),
                                          info.firstWordOrdinal, info.wordCount);
    if (!status) {
      LOG_ERR("SCT", "Failed to start PDF semantic sidecar (%u)", static_cast<unsigned>(status.error));
      return false;
    }
    sectionFirstWordOrdinal = info.firstWordOrdinal;
    sectionWordCount = info.wordCount;
    nextGlobalWordOrdinal = info.firstWordOrdinal;
    return true;
  }

  bool beginTextBlock(const char* const anchor, const size_t anchorLength) {
    wordCounter.reset();
    wordsExtractedInBlock = 0;
    blockHasToken = false;
    std::memset(currentBlockAnchor, 0, sizeof(currentBlockAnchor));
    if (anchor != nullptr && anchorLength < sizeof(currentBlockAnchor)) {
      std::memcpy(currentBlockAnchor, anchor, anchorLength);
    }
    return true;
  }

  bool trackLine(const TextBlock& line) {
    const uint32_t wordsBefore = wordCounter.words();
    bool beginsWithSplitContinuation = false;
    for (uint16_t index = 0; index < line.wordCount(); ++index) {
      const uint8_t flags = line.wordFlags(index);
      const bool attaches =
          (flags & (TextBlock::WORD_FLAG_SEMANTIC_ATTACHES | TextBlock::WORD_FLAG_SEMANTIC_SPLIT_CONTINUATION)) != 0;
      if (index == 0) {
        beginsWithSplitContinuation = (flags & TextBlock::WORD_FLAG_SEMANTIC_SPLIT_CONTINUATION) != 0;
      }
      if (blockHasToken && !attaches) {
        static constexpr uint8_t separator = ' ';
        const PdfStatus status = wordCounter.consume(&separator, 1);
        if (!status) {
          LOG_ERR("SCT", "Failed to separate PDF layout words at byte %llu",
                  static_cast<unsigned long long>(status.offset));
          return false;
        }
      }
      const PdfStatus status =
          wordCounter.consume(reinterpret_cast<const uint8_t*>(line.wordText(index)), line.wordTextLen(index));
      if (!status) {
        LOG_ERR("SCT", "Failed to count PDF layout words at byte %llu", static_cast<unsigned long long>(status.offset));
        return false;
      }
      blockHasToken = true;
    }

    const uint32_t wordsAfter = wordCounter.words();
    const uint32_t addedWords = wordsAfter - wordsBefore;
    if (addedWords == 0 && !beginsWithSplitContinuation) {
      return true;
    }
    if (addedWords > UINT32_MAX - nextGlobalWordOrdinal) {
      LOG_ERR("SCT", "PDF semantic word ordinal overflow");
      return false;
    }
    const uint32_t firstOrdinal = beginsWithSplitContinuation && nextGlobalWordOrdinal > sectionFirstWordOrdinal
                                      ? nextGlobalWordOrdinal - 1U
                                      : nextGlobalWordOrdinal;
    const uint32_t lastOrdinal = addedWords == 0 ? firstOrdinal : nextGlobalWordOrdinal + addedWords - 1U;
    if (!pageRange.valid) {
      pageRange.valid = true;
      pageRange.firstGlobalWordOrdinal = firstOrdinal;
      pageRange.firstBlockWordOffset = beginsWithSplitContinuation && wordsExtractedInBlock != 0
                                           ? wordsExtractedInBlock - 1U
                                           : wordsExtractedInBlock;
      std::memcpy(pageRange.blockAnchor, currentBlockAnchor, sizeof(pageRange.blockAnchor));
    }
    pageRange.lastGlobalWordOrdinal = lastOrdinal;
    nextGlobalWordOrdinal += addedWords;
    wordsExtractedInBlock = wordsAfter;
    return true;
  }

  bool scanNewPageLines(const Page* const page) {
    if (page == nullptr) {
      return true;
    }
    if (scannedPageElements > page->elements.size()) {
      LOG_ERR("SCT", "PDF semantic page cursor moved backwards");
      return false;
    }
    for (size_t index = scannedPageElements; index < page->elements.size(); ++index) {
      const auto& element = page->elements[index];
      if (element && element->getTag() == TAG_PageLine) {
        const auto& line = static_cast<const PageLine&>(*element);
        if (line.getBlock() && !trackLine(*line.getBlock())) {
          return false;
        }
      }
    }
    scannedPageElements = page->elements.size();
    return true;
  }

  bool finishTextBlock(const Page* const page) {
    if (!scanNewPageLines(page)) {
      return false;
    }
    if (blockHasToken) {
      const PdfStatus status = wordCounter.finish();
      if (!status) {
        LOG_ERR("SCT", "Failed to finalize PDF word tracking at byte %llu",
                static_cast<unsigned long long>(status.offset));
        return false;
      }
    }
    return true;
  }

  bool preparePage(const Page* const page) {
    if (page == nullptr || !scanNewPageLines(page)) {
      return false;
    }
    pageRange.wordCursor = nextGlobalWordOrdinal;
    return true;
  }

  bool appendPage(const PdfLayoutPageRecord& page) {
    const PdfStatus status = writer.append(pageRange, page);
    if (!status) {
      LOG_ERR("SCT", "Failed PDF semantic page %u (%u): range=%lu-%lu section=%lu-%lu cursor=%lu", writer.pageCount(),
              static_cast<unsigned>(status.error), static_cast<unsigned long>(pageRange.firstGlobalWordOrdinal),
              static_cast<unsigned long>(pageRange.lastGlobalWordOrdinal),
              static_cast<unsigned long>(sectionFirstWordOrdinal),
              static_cast<unsigned long>(sectionFirstWordOrdinal + sectionWordCount),
              static_cast<unsigned long>(nextGlobalWordOrdinal));
      return false;
    }
    pageRange = {};
    scannedPageElements = 0;
    return true;
  }

  bool finishSidecar() {
    const PdfStatus status = writer.finish();
    sectionBinding.token = writer.pairToken();
    if (!status || sectionBinding.token == 0 || !sidecarFile.sync()) {
      LOG_ERR("SCT", "Failed to finalize PDF semantic sidecar (%u) at word %llu (layout=%lu expected=%lu)",
              static_cast<unsigned>(status.error), static_cast<unsigned long long>(status.offset),
              static_cast<unsigned long>(nextGlobalWordOrdinal),
              static_cast<unsigned long>(sectionFirstWordOrdinal + sectionWordCount));
      return false;
    }
    sidecarFile.close();
    if (!Storage.openFileForRead("SCT", tempPath, sidecarFile)) {
      LOG_ERR("SCT", "Failed to reopen PDF semantic sidecar for validation");
      return false;
    }
    const PdfStatus inspect = pdfInspectLayoutWordIndex(pdfHalByteSource(sidecarFile), &replayInfo);
    if (!inspect || replayInfo.sectionIndex != static_cast<uint16_t>(section.sectionIndex) ||
        replayInfo.pageCount != section.pageCount || replayInfo.firstGlobalWordOrdinal != sectionFirstWordOrdinal) {
      LOG_ERR("SCT", "PDF semantic sidecar failed pre-promotion validation (%u)", static_cast<unsigned>(inspect.error));
      return false;
    }
    if (replayInfo.sectionWordCount != sectionWordCount) {
      LOG_ERR("SCT", "PDF semantic sidecar word count changed during build");
      return false;
    }
    return true;
  }

  bool appendSectionBindingTrailer(HalFile& destination, const uint32_t prefixLength) {
    sectionBinding.length = prefixLength;
    uint8_t trailer[PDF_LAYOUT_CACHE_BINDING_TRAILER_BYTES];
    const PdfStatus status = pdfEncodeLayoutCacheBindingTrailer(sectionBinding, trailer);
    if (!status || !destination.seek(prefixLength) || destination.write(trailer, sizeof(trailer)) != sizeof(trailer)) {
      LOG_ERR("SCT", "Failed to append PDF section binding trailer (%u)", static_cast<unsigned>(status.error));
      return false;
    }
    return true;
  }

  bool bindSidecarToSection() {
    sidecarFile.close();
    sidecarFile = Storage.open(tempPath, O_RDWR);
    if (!sidecarFile) {
      LOG_ERR("SCT", "Failed to reopen PDF semantic sidecar for binding");
      return false;
    }
    PdfStatus status =
        pdfBindLayoutWordIndex(pdfHalByteSource(sidecarFile), {&sidecarFile, patchLayoutWordIndex}, sectionBinding);
    if (!status || !sidecarFile.sync()) {
      LOG_ERR("SCT", "Failed to bind PDF semantic sidecar to section cache (%u)", static_cast<unsigned>(status.error));
      return false;
    }
    PdfLayoutWordIndexInfo boundInfo;
    status = pdfInspectLayoutWordIndex(pdfHalByteSource(sidecarFile), &boundInfo);
    if (!status || !pdfLayoutWordIndexMatchesSectionCache(boundInfo, sectionBinding) ||
        boundInfo.sectionIndex != replayInfo.sectionIndex || boundInfo.pageCount != replayInfo.pageCount) {
      LOG_ERR("SCT", "Bound PDF semantic sidecar failed temp-pair validation (%u)",
              static_cast<unsigned>(status.error));
      return false;
    }
    replayInfo = boundInfo;
    sidecarFile.close();
    return true;
  }

  bool promotePair(const char* const sectionTempPath, const char* const sectionFinalPath) {
    sidecarFile.close();
    bool oldPairInvalidated = false;
    if (Storage.exists(finalPath)) {
      if (!Storage.remove(finalPath)) {
        LOG_ERR("SCT", "Failed to invalidate prior PDF semantic sidecar");
        return false;
      }
      oldPairInvalidated = true;
    }
    if (Storage.exists(sectionFinalPath)) {
      if (!Storage.remove(sectionFinalPath)) {
        LOG_ERR("SCT", "Failed to invalidate prior PDF section cache");
        if (oldPairInvalidated) Storage.remove(sectionFinalPath);
        return false;
      }
      oldPairInvalidated = true;
    }
    if (!Storage.rename(sectionTempPath, sectionFinalPath)) {
      LOG_ERR("SCT", "Failed to promote temp PDF section cache into place");
      if (oldPairInvalidated) Storage.remove(sectionFinalPath);
      return false;
    }
    if (!Storage.rename(tempPath, finalPath)) {
      LOG_ERR("SCT", "Failed to promote PDF semantic sidecar into place");
      Storage.remove(sectionFinalPath);
      return false;
    }
    promoted = true;
    return true;
  }
};

void Section::completePdfPage(void* const context, std::unique_ptr<Page> page, const uint16_t paragraphIndex,
                              const uint16_t listItemIndex, const uint32_t visibleTextOffset) {
  auto& build = *static_cast<PdfPageBuildContext*>(context);
  if (build.pageCompletionFailed) {
    return;
  }
  if (!build.preparePage(page.get())) {
    build.pageCompletionFailed = true;
    return;
  }
  const auto prepareResult = build.pageIndex.prepareAppend();
  if (prepareResult == SectionPageIndex::PrepareResult::EntryLimit) {
    LOG_ERR("SCT", "Section page count exceeded cache format limit");
    build.pageCompletionFailed = true;
    return;
  }
  if (prepareResult == SectionPageIndex::PrepareResult::AllocationFailure) {
    LOG_ERR("SCT", "Failed to allocate section page index storage at %u entries",
            static_cast<unsigned>(build.pageIndex.size()));
    if (build.layoutAbortedForLowMemory) *build.layoutAbortedForLowMemory = true;
    build.pageCompletionFailed = true;
    return;
  }
  const uint32_t fileOffset = build.section.onPageComplete(std::move(page));
  if (fileOffset == 0) {
    build.pageCompletionFailed = true;
    return;
  }
  build.pageIndex.appendPrepared({fileOffset, paragraphIndex, listItemIndex, visibleTextOffset});
  if (!build.appendPage({fileOffset, paragraphIndex, listItemIndex})) {
    build.pageCompletionFailed = true;
  }
}

bool Section::finishPdfTextBlock(void* const context, const Page* const currentPage) {
  return static_cast<PdfPageBuildContext*>(context)->finishTextBlock(currentPage);
}

bool Section::beginPdfTextBlock(void* const context, const char* const anchor, const size_t anchorLength) {
  return static_cast<PdfPageBuildContext*>(context)->beginTextBlock(anchor, anchorLength);
}

bool Section::trackPdfTextLine(void* const context, const TextBlock* const line) {
  return line != nullptr && static_cast<PdfPageBuildContext*>(context)->trackLine(*line);
}

Section::Section(const std::shared_ptr<ReflowDocument>& document, const int sectionIndex, GfxRenderer& renderer,
                 const char* cacheSuffix)
    : document(document),
      sectionIndex(sectionIndex),
      renderer(renderer),
      filePath(document->getCachePath() + "/sections/" + std::to_string(sectionIndex) +
               (cacheSuffix ? cacheSuffix : "") + ".bin") {
  recoverSectionCacheBackup(filePath);
}

Section::Section(ReflowDocument& document, const int sectionIndex, GfxRenderer& renderer, const char* cacheSuffix)
    // Non-owning alias for stack-allocated documents (sleep page redraw, sync probes).
    : Section(std::shared_ptr<ReflowDocument>(std::shared_ptr<ReflowDocument>(), &document), sectionIndex, renderer,
              cacheSuffix) {}

// Suspend any in-progress build so every section.reset() / navigation / sleep path
// persists the pages already laid out as a partial .bin instead of discarding them
// (no-op once a build has completed or never started).
Section::~Section() { suspendBuild(); }

bool Section::usesPdfWordIndex() const {
  // PDF footnote sections ("_fn_" cache suffix) are transient and never get a sidecar.
  return document->getFormat() == ReflowDocumentFormat::Pdf && filePath.find("_fn_") == std::string::npos;
}

uint32_t Section::onPageComplete(std::unique_ptr<Page> page) {
  if (!ensureBuildFileOpen()) {
    LOG_ERR("SCT", "File not open for writing page %d", builtPageCount_);
    return 0;
  }
  if (!page) {
    LOG_ERR("SCT", "Cannot write null page %d", pageCount);
    return 0;
  }

  const uint32_t position = file.position();
  // Scan the page before serializing it so image-only and mixed pages can be
  // protected from the later XHTML byte-density projection without changing
  // the serialized page payload.
  const uint16_t imageUnits = page->imageEstimateUnits(imageEstimateViewportHeight_);
  if (!page->serialize(file)) {
    LOG_ERR("SCT", "Failed to serialize page %d", builtPageCount_);
    return 0;
  }

  protectedImageUnits_ += imageUnits;
  builtPageCount_++;
  // pageCount is the pages available to read: a rebuild over a partial only raises it
  // once it has laid out more pages than the partial already covers.
  if (builtPageCount_ > pageCount) {
    pageCount = builtPageCount_;
  }
  return position;
}

bool Section::ensureBuildFileOpen() {
  if (file) {
    return true;
  }
  const std::string& tmpSectionPath =
      build_ && !build_->tmpSectionPath.empty() ? build_->tmpSectionPath : activeBuildTmpSectionPath_;
  if (tmpSectionPath.empty()) {
    return false;
  }
  file = Storage.open(tmpSectionPath.c_str(), O_RDWR);
  if (!file) {
    LOG_ERR("SCT", "Failed to reopen section temp file");
    return false;
  }
  if (!file.seek(file.size())) {
    LOG_ERR("SCT", "Failed to seek section temp file");
    file.close();
    return false;
  }
  return true;
}

void Section::releaseBuildFile() {
  if (!build_) {
    return;
  }
  if (file) {
    file.flush();
    if (!file.sync()) {
      LOG_ERR("SCT", "Failed to sync incremental section temp file before release");
    }
    if (!file.close()) {
      LOG_ERR("SCT", "Failed to close incremental section temp file before progress save");
    }
  }
  if (build_->parser) {
    build_->parser->releaseInputFile();
  }
}

bool Section::writeSectionFileHeader(const ReaderRenderSpec& spec) {
  if (!file) {
    LOG_DBG("SCT", "File not open for writing header");
    return false;
  }
  static_assert(HEADER_SIZE == sizeof(SECTION_CACHE_MAGIC) + sizeof(SECTION_FILE_VERSION) + sizeof(spec.fontId) +
                                   sizeof(spec.lineCompression) + sizeof(spec.extraParagraphSpacing) +
                                   sizeof(spec.forceParagraphIndents) + sizeof(spec.paragraphAlignment) +
                                   sizeof(spec.viewportWidth) + sizeof(spec.viewportHeight) +
                                   sizeof(spec.hyphenationEnabled) + sizeof(spec.embeddedStyle) +
                                   sizeof(spec.imageRendering) + sizeof(spec.focusReadingEnabled) +
                                   sizeof(spec.guideReadingEnabled) + sizeof(spec.wordSpacing) + sizeof(uint8_t) +
                                   sizeof(pageCount) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) +
                                   sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t),
                "Header size mismatch");
  return serialization::tryWritePod(file, SECTION_CACHE_MAGIC) &&
         serialization::tryWritePod(file, SECTION_FILE_VERSION) && serialization::tryWritePod(file, spec.fontId) &&
         serialization::tryWritePod(file, spec.lineCompression) &&
         serialization::tryWritePod(file, spec.extraParagraphSpacing) &&
         serialization::tryWritePod(file, spec.forceParagraphIndents) &&
         serialization::tryWritePod(file, spec.paragraphAlignment) &&
         serialization::tryWritePod(file, spec.viewportWidth) &&
         serialization::tryWritePod(file, spec.viewportHeight) &&
         serialization::tryWritePod(file, spec.hyphenationEnabled) &&
         serialization::tryWritePod(file, spec.embeddedStyle) &&
         serialization::tryWritePod(file, spec.imageRendering) &&
         serialization::tryWritePod(file, spec.focusReadingEnabled) &&
         serialization::tryWritePod(file, spec.guideReadingEnabled) &&
         serialization::tryWritePod(file, spec.wordSpacing) &&
         serialization::tryWritePod(file, static_cast<uint8_t>(spec.renderMode)) &&
         serialization::tryWritePod(file,
                                    pageCount) &&  // Placeholder for page count (will be initially 0, patched later)
         serialization::tryWritePod(file, static_cast<uint32_t>(0)) &&  // Protected image units (patched later)
         serialization::tryWritePod(file, static_cast<uint32_t>(0)) &&  // Placeholder for LUT offset (patched later)
         serialization::tryWritePod(file,
                                    static_cast<uint32_t>(0)) &&  // Placeholder for anchor map offset (patched later)
         serialization::tryWritePod(
             file,
             static_cast<uint32_t>(0)) &&  // Placeholder for paragraph LUT offset (patched later)
         serialization::tryWritePod(file, static_cast<uint32_t>(0)) &&  // li LUT offset
         serialization::tryWritePod(file, static_cast<uint32_t>(0));    // visible text LUT offset
}

bool Section::loadSectionFile(const ReaderRenderSpec& spec) {
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return false;
  }

  // Match parameters
  bool filePartial = false;
  {
    uint32_t magic;
    if (!serialization::tryReadPod(file, magic)) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: could not read cache magic");
      clearCache();
      return false;
    }
    if (magic != SECTION_CACHE_MAGIC) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: cache magic mismatch");
      clearCache();
      return false;
    }

    uint8_t version;
    if (!serialization::tryReadPod(file, version)) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: could not read version");
      clearCache();
      return false;
    }
    if (version != SECTION_FILE_VERSION && version != SECTION_FILE_PARTIAL_VERSION) {
      // Explicit close() required: member variable persists beyond function scope
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Unknown version %u", version);
      clearCache();
      return false;
    }
    filePartial = (version == SECTION_FILE_PARTIAL_VERSION);

    int fileFontId;
    uint16_t fileViewportWidth, fileViewportHeight;
    float fileLineCompression;
    bool fileExtraParagraphSpacing;
    bool fileForceParagraphIndents;
    uint8_t fileParagraphAlignment;
    bool fileHyphenationEnabled;
    bool fileEmbeddedStyle;
    uint8_t fileImageRendering;
    bool fileFocusReadingEnabled;
    bool fileGuideReadingEnabled;
    uint8_t fileWordSpacing;
    uint8_t fileRenderMode;
    if (!serialization::tryReadPod(file, fileFontId) || !serialization::tryReadPod(file, fileLineCompression) ||
        !serialization::tryReadPod(file, fileExtraParagraphSpacing) ||
        !serialization::tryReadPod(file, fileForceParagraphIndents) ||
        !serialization::tryReadPod(file, fileParagraphAlignment) ||
        !serialization::tryReadPod(file, fileViewportWidth) || !serialization::tryReadPod(file, fileViewportHeight) ||
        !serialization::tryReadPod(file, fileHyphenationEnabled) ||
        !serialization::tryReadPod(file, fileEmbeddedStyle) || !serialization::tryReadPod(file, fileImageRendering) ||
        !serialization::tryReadPod(file, fileFocusReadingEnabled) ||
        !serialization::tryReadPod(file, fileGuideReadingEnabled) ||
        !serialization::tryReadPod(file, fileWordSpacing) || !serialization::tryReadPod(file, fileRenderMode)) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: truncated section header");
      clearCache();
      return false;
    }

    if (spec.fontId != fileFontId || spec.lineCompression != fileLineCompression ||
        spec.extraParagraphSpacing != fileExtraParagraphSpacing ||
        spec.forceParagraphIndents != fileForceParagraphIndents || spec.paragraphAlignment != fileParagraphAlignment ||
        spec.viewportWidth != fileViewportWidth || spec.viewportHeight != fileViewportHeight ||
        spec.hyphenationEnabled != fileHyphenationEnabled || spec.embeddedStyle != fileEmbeddedStyle ||
        spec.imageRendering != fileImageRendering || spec.focusReadingEnabled != fileFocusReadingEnabled ||
        spec.guideReadingEnabled != fileGuideReadingEnabled || spec.wordSpacing != fileWordSpacing ||
        static_cast<uint8_t>(spec.renderMode) != fileRenderMode) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Parameters do not match");
      clearCache();
      return false;
    }
  }

  if (!serialization::tryReadPod(file, pageCount)) {
    file.close();
    LOG_ERR("SCT", "Deserialization failed: missing page count");
    clearCache();
    return false;
  }
  if (!serialization::tryReadPod(file, partialProtectedImageUnits_)) {
    file.close();
    LOG_ERR("SCT", "Deserialization failed: missing protected image units");
    clearCache();
    pageCount = 0;
    return false;
  }

  if (filePartial) {
    // A partial's pageCount is the watermark of a suspended build. Read the watermark
    // trailer (appended after the page-start visible-text LUT) so estimatedTotalPages can extrapolate.
    uint32_t visibleTextLutOffset = 0;
    if (!file.seek(HEADER_SIZE - sizeof(uint32_t)) || !serialization::tryReadPod(file, visibleTextLutOffset)) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: missing partial trailer offset");
      clearCache();
      pageCount = 0;
      return false;
    }
    const uint32_t trailerOffset = visibleTextLutOffset + static_cast<uint32_t>(pageCount) * sizeof(uint32_t);
    const bool trailerValid =
        pageCount > 0 && visibleTextLutOffset >= HEADER_SIZE && trailerOffset + 2 * sizeof(uint32_t) <= file.size();
    if (!trailerValid) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: malformed partial section");
      clearCache();
      pageCount = 0;
      return false;
    }
    if (!file.seek(trailerOffset) || !serialization::tryReadPod(file, partialBytesConsumed_) ||
        !serialization::tryReadPod(file, partialTotalBytes_)) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: truncated partial trailer");
      clearCache();
      pageCount = 0;
      return false;
    }
    partial_ = true;
    partialPageCount_ = pageCount;
  } else {
    partial_ = false;
    partialPageCount_ = 0;
    protectedImageUnits_ = partialProtectedImageUnits_;
    partialProtectedImageUnits_ = 0;
    partialBytesConsumed_ = 0;
    partialTotalBytes_ = 0;
  }

  // Explicit close() required: member variable persists beyond function scope
  file.close();
  if (usesPdfWordIndex() && (partial_ || !document->validateLayoutWordIndex(filePath, sectionIndex, pageCount))) {
    LOG_ERR("SCT", "PDF semantic sidecar invalid for section %d", sectionIndex);
    clearCache();
    pageCount = 0;
    partial_ = false;
    partialPageCount_ = 0;
    return false;
  }
  return true;
}

// Your updated class method (assuming you are using the 'SD' object, which is a wrapper for a specific filesystem)
bool Section::clearCache() const {
  if (usesPdfWordIndex() && !document->removeLayoutWordIndex(filePath)) {
    LOG_ERR("SCT", "Failed to clear PDF semantic sidecar");
    return false;
  }
  const std::string tmpBin = binTmpPath();
  if (Storage.exists(tmpBin.c_str())) {
    Storage.remove(tmpBin.c_str());
  }
  const std::string backupPath = sectionBackupPath(filePath);
  if (Storage.exists(backupPath.c_str())) {
    Storage.remove(backupPath.c_str());
  }
  if (!Storage.exists(filePath.c_str())) {
    return true;
  }

  if (!Storage.remove(filePath.c_str())) {
    LOG_ERR("SCT", "Failed to clear cache");
    return false;
  }

  return true;
}

bool Section::createSectionFile(const ReaderRenderSpec& spec, const std::function<void()>& popupFn,
                                bool* imagesWereSuppressed, bool* layoutAbortedForLowMemory,
                                const SectionBuildOptions buildOptions) {
  const int fontId = spec.fontId;
  const float lineCompression = spec.lineCompression;
  const bool extraParagraphSpacing = spec.extraParagraphSpacing;
  const bool forceParagraphIndents = spec.forceParagraphIndents;
  const uint8_t paragraphAlignment = spec.paragraphAlignment;
  const uint16_t viewportWidth = spec.viewportWidth;
  const uint16_t viewportHeight = spec.viewportHeight;
  const bool hyphenationEnabled = spec.hyphenationEnabled;
  const bool embeddedStyle = spec.embeddedStyle;
  const uint8_t imageRendering = spec.imageRendering;
  const bool focusReadingEnabled = spec.focusReadingEnabled;
  const bool guideReadingEnabled = spec.guideReadingEnabled;
  const uint8_t wordSpacing = spec.wordSpacing;
  const EpubRenderMode renderMode = spec.renderMode;
  std::string localPath;
  if (!document->getSectionHref(sectionIndex, localPath)) {
    LOG_ERR("SCT", "Section %d has no source href", sectionIndex);
    return false;
  }
  const auto htmlDir = document->getCachePath() + "/html";
  const auto htmlPath = htmlDir + "/" + std::to_string(sectionIndex) + ".html";
  const auto tmpHtmlPath = htmlDir + "/.tmp_" + std::to_string(sectionIndex) + ".html";
  const auto tmpSectionPath = filePath + ".tmp";
  activeBuildTmpSectionPath_ = tmpSectionPath;
  struct ClearActiveBuildTmpPath {
    std::string& path;
    ~ClearActiveBuildTmpPath() { path.clear(); }
  } clearActiveBuildTmpPath{activeBuildTmpSectionPath_};
  pageCount = 0;
  builtPageCount_ = 0;
  imageEstimateViewportHeight_ = viewportHeight;
  protectedImageUnits_ = 0;
  if (layoutAbortedForLowMemory) *layoutAbortedForLowMemory = false;
  if (buildOptions.cancellationObserved) *buildOptions.cancellationObserved = false;
  const auto cancelBuild = [&buildOptions]() {
    if (!buildOptions.isCancellationRequested()) {
      return false;
    }
    if (buildOptions.cancellationObserved) {
      *buildOptions.cancellationObserved = true;
    }
    return true;
  };
  const bool effectiveFocusReadingEnabled = focusReadingEnabled;
  const bool effectiveGuideReadingEnabled = guideReadingEnabled;
  LOG_DBG("SCT",
          "Create section start: spine=%d mode=%u preview=%u viewport=%ux%u image=%u focus=%u guide=%u free=%u "
          "maxAlloc=%u",
          sectionIndex, static_cast<unsigned>(renderMode), buildOptions.isPreview() ? 1U : 0U, viewportWidth,
          viewportHeight, imageRendering, effectiveFocusReadingEnabled, effectiveGuideReadingEnabled, ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());
  MemoryBudget::logEpubHeapPools("section build start");

  // Create cache directory if it doesn't exist
  {
    const auto sectionsDir = document->getCachePath() + "/sections";
    Storage.mkdir(sectionsDir.c_str());
  }

  // PDF sections are already XHTML files inside the PDF cache; parse them in place.
  ReflowResource borrowedSection;
  const bool usesBorrowedSection = document->getImmutableLocalSection(sectionIndex, borrowedSection);

  // Reuse the previously unzipped HTML if we already have it. The unzipped HTML is keyed only on the
  // book (it lives in the per-book cache dir), not on render settings, so it survives the invalidation
  // that wipes the layout (.bin) caches when font/margin/orientation change -- rebuilds then skip zip
  // inflation entirely. It's promoted by an atomic rename as soon as the inflate succeeds (below), so
  // even a window-only giant spine -- whose .bin never finalizes -- still caches its HTML, letting a
  // reopen skip the multi-second inflate. If htmlPath exists it is known-complete.
  const bool reusedHtml = !usesBorrowedSection && Storage.exists(htmlPath.c_str());
  bool htmlCached = reusedHtml || usesBorrowedSection;
  const auto cleanupTempHtml = [&]() {
    if (!htmlCached && Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
    }
  };
  if (cancelBuild()) {
    LOG_DBG("SCT", "Section build cancelled before HTML inflate: spine=%d", sectionIndex);
    return false;
  }
  if (!reusedHtml && !usesBorrowedSection) {
    Storage.mkdir(htmlDir.c_str());

    // Retry logic for SD card timing issues
    bool streamed = false;
    uint32_t fileSize = 0;
    for (int attempt = 0; attempt < 3 && !streamed; attempt++) {
      if (attempt > 0) {
        LOG_DBG("SCT", "Retrying stream (attempt %d)...", attempt + 1);
        delay(50);  // Brief delay before retry
      }

      // Remove any incomplete file from previous attempt before retrying
      if (Storage.exists(tmpHtmlPath.c_str())) {
        Storage.remove(tmpHtmlPath.c_str());
      }

      HalFile tmpHtml;
      if (!Storage.openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) {
        continue;
      }
      const size_t htmlStreamChunkSize = sectionHtmlStreamChunkSize(buildOptions.isPreview());
      prepareSectionZipInflate(renderer, fontId);
      streamed = document->streamSection(sectionIndex, tmpHtml, htmlStreamChunkSize);
      fileSize = tmpHtml.size();
      // Explicitly close() file before calling Storage.remove()
      tmpHtml.close();

      // If streaming failed, remove the incomplete file immediately
      if (!streamed && Storage.exists(tmpHtmlPath.c_str())) {
        Storage.remove(tmpHtmlPath.c_str());
        LOG_DBG("SCT", "Removed incomplete temp file after failed attempt");
      }
    }

    if (!streamed) {
      LOG_ERR("SCT", "Failed to stream item contents to temp file after retries");
      return false;
    }

    // Promote to the persistent HTML cache immediately -- the inflate is complete and the bytes are
    // valid regardless of whether the layout build finishes, so reopening (even a window-only spine
    // that never finalizes its .bin) skips re-inflation. If the rename fails we just parse the temp.
    if (Storage.rename(tmpHtmlPath.c_str(), htmlPath.c_str())) {
      htmlCached = true;
    } else {
      LOG_DBG("SCT", "Failed to promote HTML cache; parsing from temp");
    }
  }
  const std::string& parsePath =
      usesBorrowedSection ? borrowedSection.localPath : (htmlCached ? htmlPath : tmpHtmlPath);

  if (cancelBuild()) {
    LOG_DBG("SCT", "Section build cancelled after HTML inflate: spine=%d", sectionIndex);
    cleanupTempHtml();
    return false;
  }

  if (Storage.exists(tmpSectionPath.c_str())) {
    Storage.remove(tmpSectionPath.c_str());
  }

  if (!Storage.openFileForWrite("SCT", tmpSectionPath, file)) {
    cleanupTempHtml();
    return false;
  }
  ReaderRenderSpec effectiveSpec = spec;
  effectiveSpec.focusReadingEnabled = effectiveFocusReadingEnabled;
  effectiveSpec.guideReadingEnabled = effectiveGuideReadingEnabled;
  if (!writeSectionFileHeader(effectiveSpec)) {
    LOG_ERR("SCT", "Failed to write section header");
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    cleanupTempHtml();
    return false;
  }
  SectionPageIndex pageIndex;
  bool pageCompletionFailed = false;
  const bool semanticPositionEnabled = usesPdfWordIndex() && !buildOptions.isPreview();

  // Derive the content base directory and image cache path prefix for the parser. PDF
  // image references resolve against the borrowed section's physical cache directory.
  const bool usesPhysicalBorrowedContentPath =
      usesBorrowedSection && document->getFormat() == ReflowDocumentFormat::Pdf;
  const std::string& contentPath = usesPhysicalBorrowedContentPath ? borrowedSection.localPath : localPath;
  size_t lastSlash = contentPath.find_last_of('/');
  std::string contentBase = (lastSlash != std::string::npos) ? contentPath.substr(0, lastSlash + 1) : "";
  std::string imageBasePath = document->getCachePath() + "/img_" + std::to_string(sectionIndex) + "_";

  CssParser* cssParser = nullptr;
  if (embeddedStyle) {
    cssParser = document->getCssParser();
    if (cssParser) {
      const auto cssHeapBefore = MemoryBudget::snapshot();
      const bool cssLoaded = cssParser->loadFromCache();
      const auto cssHeapAfter = MemoryBudget::snapshot();
      LOG_DBG("SCT", "CSS cache load: ok=%u partial=%u rules=%u free=%u->%u delta=%d maxAlloc=%u->%u delta=%d",
              cssLoaded ? 1U : 0U, cssParser->isCachePartial() ? 1U : 0U, static_cast<unsigned>(cssParser->ruleCount()),
              cssHeapBefore.freeHeap, cssHeapAfter.freeHeap,
              static_cast<int32_t>(cssHeapAfter.freeHeap) - static_cast<int32_t>(cssHeapBefore.freeHeap),
              cssHeapBefore.maxAllocHeap, cssHeapAfter.maxAllocHeap,
              static_cast<int32_t>(cssHeapAfter.maxAllocHeap) - static_cast<int32_t>(cssHeapBefore.maxAllocHeap));
      if (!cssLoaded) {
        LOG_ERR("SCT", "Failed to load CSS from cache");
      }
    }
  }

  // Collect TOC anchors for this spine so the parser can insert page breaks at chapter boundaries
  std::vector<std::string> tocAnchors;
  const int startTocIndex = buildOptions.isPreview() ? -1 : document->getTocIndexForSectionIndex(sectionIndex);
  if (startTocIndex >= 0) {
    for (int i = startTocIndex; i < document->getTocEntryCount(); i++) {
      auto entry = document->getTocEntry(i);
      if (entry.sectionIndex != sectionIndex) break;
      if (!entry.anchor.empty()) {
        tocAnchors.push_back(std::move(entry.anchor));
      }
    }
  }

  std::unique_ptr<PdfPageBuildContext> pdfBuild;
  std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t)> completePage;
  ChapterHtmlPaginationHooks paginationHooks;
  if (semanticPositionEnabled) {
    const ReflowSectionInfo sectionInfo = document->getSectionInfo(sectionIndex);
    // One PDF-only allocation keeps the sidecar writer, word counter, and fixed path
    // buffers alive across parser callbacks without enlarging Section or the parser.
    pdfBuild =
        makeUniqueNoThrow<PdfPageBuildContext>(*this, pageCompletionFailed, pageIndex, layoutAbortedForLowMemory);
    if (!pdfBuild || !pdfBuild->initialize(filePath, sectionIndex, sectionInfo)) {
      LOG_ERR("SCT", "Failed to allocate or initialize PDF semantic build context");
      if (!pdfBuild && layoutAbortedForLowMemory) *layoutAbortedForLowMemory = true;
      file.close();
      Storage.remove(tmpSectionPath.c_str());
      cleanupTempHtml();
      return false;
    }
    static const ChapterHtmlPaginationVtable pdfPaginationVtable = {
        completePdfPage,
        finishPdfTextBlock,
        beginPdfTextBlock,
        trackPdfTextLine,
    };
    paginationHooks = {pdfBuild.get(), &pdfPaginationVtable};
  } else {
    completePage = [this, &pageIndex, &pageCompletionFailed, layoutAbortedForLowMemory](
                       std::unique_ptr<Page> page, const uint16_t paragraphIndex, const uint16_t listItemIndex,
                       const uint32_t visibleTextOffset) {
      if (pageCompletionFailed) {
        return;
      }
      const auto prepareResult = pageIndex.prepareAppend();
      if (prepareResult == SectionPageIndex::PrepareResult::EntryLimit) {
        LOG_ERR("SCT", "Section page count exceeded cache format limit");
        pageCompletionFailed = true;
        return;
      }
      if (prepareResult == SectionPageIndex::PrepareResult::AllocationFailure) {
        LOG_ERR("SCT", "Failed to allocate section page index storage at %u entries",
                static_cast<unsigned>(pageIndex.size()));
        if (layoutAbortedForLowMemory) *layoutAbortedForLowMemory = true;
        pageCompletionFailed = true;
        return;
      }
      const uint32_t fileOffset = this->onPageComplete(std::move(page));
      if (fileOffset == 0) {
        pageCompletionFailed = true;
        return;
      }
      pageIndex.appendPrepared({fileOffset, paragraphIndex, listItemIndex, visibleTextOffset});
    };
  }

  ChapterHtmlSlimParser visitor(
      *document, sectionIndex, parsePath, renderer, fontId, lineCompression, extraParagraphSpacing,
      forceParagraphIndents, paragraphAlignment, viewportWidth, viewportHeight, hyphenationEnabled,
      effectiveFocusReadingEnabled, effectiveGuideReadingEnabled, wordSpacing, std::move(completePage), embeddedStyle,
      contentBase, imageBasePath, imageRendering, std::move(tocAnchors), popupFn, cssParser, renderMode,
      buildOptions.isPreview() ? std::string(buildOptions.previewAnchor) : std::string{}, buildOptions.previewMaxPages,
      paginationHooks, usesPhysicalBorrowedContentPath);
  Hyphenator::setPreferredLanguage(document->getLanguage());
  bool cancelled = false;
  bool success = false;
  if (cancelBuild()) {
    cancelled = true;
  } else {
    success = visitor.beginParse();
  }
  while (success && !cancelled) {
    if (cancelBuild()) {
      visitor.abortParse();
      cancelled = true;
      break;
    }
    const auto status = visitor.parseStep();
    if (status == ChapterHtmlSlimParser::ParseStatus::Error) {
      visitor.abortParse();
      success = false;
      break;
    }
    if (status == ChapterHtmlSlimParser::ParseStatus::Done) {
      if (cancelBuild()) {
        visitor.abortParse();
        cancelled = true;
      } else {
        success = visitor.finishParse();
      }
      break;
    }
  }
  LOG_DBG("SCT", "Parser done: spine=%d success=%u pages=%u free=%u maxAlloc=%u", sectionIndex, success, pageCount,
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  if (imagesWereSuppressed) *imagesWereSuppressed = visitor.wasLowMemoryFallbackTriggered();
  if (layoutAbortedForLowMemory) {
    *layoutAbortedForLowMemory = *layoutAbortedForLowMemory || visitor.wasLowMemoryAbortTriggered();
  }

  if (!htmlCached) {
    if (success || pageCompletionFailed) {
      // Promote the freshly unzipped HTML to the persistent cache so future rebuilds (e.g. after a
      // settings change invalidates the layout caches) can skip zip inflation. If promotion fails,
      // drop the temp file; the section build can still continue from the already-open source.
      if (!Storage.rename(tmpHtmlPath.c_str(), htmlPath.c_str())) {
        LOG_DBG("SCT", "Failed to promote HTML cache, removing temp");
        Storage.remove(tmpHtmlPath.c_str());
      }
    } else {
      // Parse failed on a freshly unzipped file -- discard it rather than caching a bad source.
      Storage.remove(tmpHtmlPath.c_str());
    }
  }

  if (pdfBuild && success && !pageCompletionFailed && !cancelled && !pdfBuild->finishSidecar()) {
    pageCompletionFailed = true;
  }

  if (!success || pageCompletionFailed || cancelled) {
    if (cancelled) {
      LOG_DBG("SCT", "Section build cancelled during parse: spine=%d", sectionIndex);
    } else {
      LOG_ERR("SCT", "Failed to parse XML and build pages");
    }
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }

  const auto& anchors = visitor.getAnchors();
  SectionPageIndexOffsets pageIndexOffsets;
  const bool wrotePageIndex = writeSectionPageIndex(
      file, pageIndex,
      [&anchors](FsFile& output) {
        if (!serialization::tryWritePod(output, static_cast<uint16_t>(anchors.size()))) return false;
        for (const auto& [anchor, page] : anchors) {
          if (!serialization::tryWriteString(output, anchor) || !serialization::tryWritePod(output, page)) return false;
        }
        return true;
      },
      pageIndexOffsets);
  if (!wrotePageIndex) {
    LOG_ERR("SCT", "Failed to write section page index");
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    return false;
  }
  // PDF sections bind their word-index sidecar to this exact prefix via a trailer.
  const uint32_t sectionPrefixLength = static_cast<uint32_t>(file.position());

  // Patch header with final pageCount and all cache lookup-table offsets.
  if (!file.seek(HEADER_SIZE - sizeof(uint32_t) * 6 - sizeof(pageCount)) ||
      !serialization::tryWritePod(file, pageCount) || !serialization::tryWritePod(file, protectedImageUnits_) ||
      !serialization::tryWritePod(file, pageIndexOffsets.page) ||
      !serialization::tryWritePod(file, pageIndexOffsets.anchorMap) ||
      !serialization::tryWritePod(file, pageIndexOffsets.paragraph) ||
      !serialization::tryWritePod(file, pageIndexOffsets.listItem) ||
      !serialization::tryWritePod(file, pageIndexOffsets.visibleText) ||
      (pdfBuild && !pdfBuild->appendSectionBindingTrailer(file, sectionPrefixLength)) || !file.sync()) {
    LOG_ERR("SCT", "Failed to finalize section cache");
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  const bool promoted =
      pdfBuild ? pdfBuild->bindSidecarToSection() && pdfBuild->promotePair(tmpSectionPath.c_str(), filePath.c_str())
               : promoteSectionCache(tmpSectionPath, filePath);
  if (!promoted) {
    LOG_ERR("SCT", "Failed to promote temp section cache into place");
    Storage.remove(tmpSectionPath.c_str());
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }
  if (pdfBuild && !document->validateLayoutWordIndex(filePath, sectionIndex, pageCount)) {
    LOG_ERR("SCT", "Promoted PDF semantic sidecar failed validation");
    Storage.remove(filePath.c_str());
    document->removeLayoutWordIndex(filePath);
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }
  if (cssParser) {
    cssParser->clear();
  }
  partial_ = false;
  partialPageCount_ = 0;
  partialProtectedImageUnits_ = 0;
  return true;
}

bool Section::startBuild(const ReaderRenderSpec& spec, const SectionBuildOptions buildOptions,
                         const std::function<void()>& popupFn) {
  const int fontId = spec.fontId;
  const float lineCompression = spec.lineCompression;
  const bool extraParagraphSpacing = spec.extraParagraphSpacing;
  const bool forceParagraphIndents = spec.forceParagraphIndents;
  const uint8_t paragraphAlignment = spec.paragraphAlignment;
  const uint16_t viewportWidth = spec.viewportWidth;
  const uint16_t viewportHeight = spec.viewportHeight;
  const bool hyphenationEnabled = spec.hyphenationEnabled;
  const bool embeddedStyle = spec.embeddedStyle;
  const uint8_t imageRendering = spec.imageRendering;
  const bool focusReadingEnabled = spec.focusReadingEnabled;
  const bool guideReadingEnabled = spec.guideReadingEnabled;
  const uint8_t wordSpacing = spec.wordSpacing;
  const EpubRenderMode renderMode = spec.renderMode;
  if (build_) {
    LOG_ERR("SCT", "startBuild called while a build is already active");
    return false;
  }
  if (usesPdfWordIndex()) {
    LOG_ERR("SCT", "Incremental builds are unsupported for PDF sections; use createSectionFile");
    return false;
  }

  std::string localPath;
  if (!document->getSectionHref(sectionIndex, localPath)) {
    LOG_ERR("SCT", "Section %d has no source href", sectionIndex);
    return false;
  }
  const auto htmlDir = document->getCachePath() + "/html";
  const auto htmlPath = htmlDir + "/" + std::to_string(sectionIndex) + ".html";
  const auto tmpHtmlPath = htmlDir + "/.tmp_" + std::to_string(sectionIndex) + ".html";
  const auto tmpSectionPath = binTmpPath();
  builtPageCount_ = 0;
  imageEstimateViewportHeight_ = viewportHeight;
  protectedImageUnits_ = 0;
  pageCount = partial_ ? partialPageCount_ : 0;
  buildComplete_ = false;
  lastImagesWereSuppressed_ = false;
  lastLayoutAbortedForLowMemory_ = false;

  if (Storage.exists(tmpSectionPath.c_str())) {
    Storage.remove(tmpSectionPath.c_str());
  }

  LOG_DBG("SCT",
          "Start incremental section build: spine=%d mode=%u preview=%u viewport=%ux%u image=%u focus=%u guide=%u "
          "free=%u maxAlloc=%u",
          sectionIndex, static_cast<unsigned>(renderMode), buildOptions.isPreview() ? 1U : 0U, viewportWidth,
          viewportHeight, imageRendering, focusReadingEnabled, guideReadingEnabled, ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());

  {
    const auto sectionsDir = document->getCachePath() + "/sections";
    Storage.mkdir(sectionsDir.c_str());
  }

  const bool reusedHtml = Storage.exists(htmlPath.c_str());
  bool htmlCached = reusedHtml;
  const auto cleanupTempHtml = [&]() {
    if (!htmlCached && Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
    }
  };
  if (!reusedHtml) {
    Storage.mkdir(htmlDir.c_str());

    bool streamed = false;
    uint32_t fileSize = 0;
    for (int attempt = 0; attempt < 3 && !streamed; attempt++) {
      if (attempt > 0) {
        LOG_DBG("SCT", "Retrying stream (attempt %d)...", attempt + 1);
        delay(50);
      }
      if (Storage.exists(tmpHtmlPath.c_str())) {
        Storage.remove(tmpHtmlPath.c_str());
      }

      HalFile tmpHtml;
      if (!Storage.openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) {
        continue;
      }
      const size_t htmlStreamChunkSize = sectionHtmlStreamChunkSize(buildOptions.isPreview());
      prepareSectionZipInflate(renderer, fontId);
      streamed = document->streamSection(sectionIndex, tmpHtml, htmlStreamChunkSize);
      fileSize = tmpHtml.size();
      tmpHtml.close();
      if (!streamed && Storage.exists(tmpHtmlPath.c_str())) {
        Storage.remove(tmpHtmlPath.c_str());
      }
    }

    if (!streamed) {
      LOG_ERR("SCT", "Failed to stream item contents to temp file after retries");
      return false;
    }
    if (Storage.rename(tmpHtmlPath.c_str(), htmlPath.c_str())) {
      htmlCached = true;
    } else {
      LOG_DBG("SCT", "Failed to promote HTML cache; parsing from temp");
    }
  }

  if (!Storage.openFileForWrite("SCT", tmpSectionPath, file)) {
    cleanupTempHtml();
    return false;
  }
  if (!writeSectionFileHeader(spec)) {
    LOG_ERR("SCT", "Failed to write section header");
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    cleanupTempHtml();
    return false;
  }

  auto ctx = makeUniqueNoThrow<BuildContext>();
  if (!ctx) {
    LOG_ERR("SCT", "Failed to allocate section build context");
    lastLayoutAbortedForLowMemory_ = true;
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    cleanupTempHtml();
    return false;
  }
  ctx->reusedHtml = htmlCached;
  ctx->htmlPath = htmlPath;
  ctx->tmpHtmlPath = tmpHtmlPath;
  ctx->tmpSectionPath = tmpSectionPath;
  ctx->parsePath = htmlCached ? htmlPath : tmpHtmlPath;

  const size_t lastSlash = localPath.find_last_of('/');
  ctx->contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  ctx->imageBasePath = document->getCachePath() + "/img_" + std::to_string(sectionIndex) + "_";

  if (embeddedStyle) {
    ctx->cssParser = document->getCssParser();
    if (ctx->cssParser) {
      const auto cssHeapBefore = MemoryBudget::snapshot();
      const bool cssLoaded = ctx->cssParser->loadFromCache();
      const auto cssHeapAfter = MemoryBudget::snapshot();
      LOG_DBG("SCT", "CSS cache load: ok=%u partial=%u rules=%u free=%u->%u delta=%d maxAlloc=%u->%u delta=%d",
              cssLoaded ? 1U : 0U, ctx->cssParser->isCachePartial() ? 1U : 0U,
              static_cast<unsigned>(ctx->cssParser->ruleCount()), cssHeapBefore.freeHeap, cssHeapAfter.freeHeap,
              static_cast<int32_t>(cssHeapAfter.freeHeap) - static_cast<int32_t>(cssHeapBefore.freeHeap),
              cssHeapBefore.maxAllocHeap, cssHeapAfter.maxAllocHeap,
              static_cast<int32_t>(cssHeapAfter.maxAllocHeap) - static_cast<int32_t>(cssHeapBefore.maxAllocHeap));
      if (!cssLoaded) {
        LOG_ERR("SCT", "Failed to load CSS from cache");
      }
    }
  }

  std::vector<std::string> tocAnchors;
  const int startTocIndex = buildOptions.isPreview() ? -1 : document->getTocIndexForSectionIndex(sectionIndex);
  if (startTocIndex >= 0) {
    for (int i = startTocIndex; i < document->getTocEntryCount(); i++) {
      auto entry = document->getTocEntry(i);
      if (entry.sectionIndex != sectionIndex) break;
      if (!entry.anchor.empty()) {
        tocAnchors.push_back(std::move(entry.anchor));
      }
    }
  }

  BuildContext* ctxPtr = ctx.get();
  ctx->parser = makeUniqueNoThrow<ChapterHtmlSlimParser>(
      *document, sectionIndex, ctxPtr->parsePath, renderer, fontId, lineCompression, extraParagraphSpacing,
      forceParagraphIndents, paragraphAlignment, viewportWidth, viewportHeight, hyphenationEnabled, focusReadingEnabled,
      guideReadingEnabled, wordSpacing,
      [this, ctxPtr](std::unique_ptr<Page> page, const uint16_t paragraphIndex, const uint16_t listItemIndex,
                     const uint32_t visibleTextOffset) {
        if (ctxPtr->pageCompletionFailed) {
          return;
        }
        const auto prepareResult = ctxPtr->pageIndex.prepareAppend();
        if (prepareResult == SectionPageIndex::PrepareResult::EntryLimit) {
          LOG_ERR("SCT", "Incremental section page count exceeded cache format limit");
          ctxPtr->pageCompletionFailed = true;
          return;
        }
        if (prepareResult == SectionPageIndex::PrepareResult::AllocationFailure) {
          LOG_ERR("SCT", "Failed to allocate incremental page index storage at %u entries",
                  static_cast<unsigned>(ctxPtr->pageIndex.size()));
          ctxPtr->pageCompletionFailed = true;
          lastLayoutAbortedForLowMemory_ = true;
          return;
        }
        const uint32_t fileOffset = this->onPageComplete(std::move(page));
        if (fileOffset == 0) {
          ctxPtr->pageCompletionFailed = true;
          return;
        }
        ctxPtr->pageIndex.appendPrepared({fileOffset, paragraphIndex, listItemIndex, visibleTextOffset});
      },
      embeddedStyle, ctxPtr->contentBase, ctxPtr->imageBasePath, imageRendering, std::move(tocAnchors), popupFn,
      ctxPtr->cssParser, renderMode, buildOptions.isPreview() ? std::string(buildOptions.previewAnchor) : std::string{},
      buildOptions.previewMaxPages);
  if (!ctx->parser) {
    LOG_ERR("SCT", "Failed to allocate section parser");
    lastLayoutAbortedForLowMemory_ = true;
    if (ctx->cssParser) ctx->cssParser->clear();
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    cleanupTempHtml();
    return false;
  }

  Hyphenator::setPreferredLanguage(document->getLanguage());
  build_ = std::move(ctx);
  if (!build_->parser->beginParse()) {
    LOG_ERR("SCT", "Failed to begin incremental section parse");
    lastLayoutAbortedForLowMemory_ = build_->parser->wasLowMemoryAbortTriggered();
    if (lastLayoutAbortedForLowMemory_ && partial_) {
      suspendBuild();
    } else {
      abandonBuild();
    }
    return false;
  }
  build_->totalBytes = build_->parser->parseTotalBytes();
  return true;
}

bool Section::buildSomeMore(const int maxPages) {
  if (!build_ || !build_->parser) {
    LOG_ERR("SCT", "buildSomeMore called with no active build");
    return false;
  }
  // Pace on pages laid out by THIS build, not pageCount: during a rebuild over a partial,
  // pageCount stays pinned at the partial's watermark until the build passes it, which
  // would otherwise turn one "small" chunk into a blocking rebuild of the whole watermark.
  const int startCount = builtPageCount_;
  for (;;) {
    const auto status = build_->parser->parseStep();
    lastImagesWereSuppressed_ = lastImagesWereSuppressed_ || build_->parser->wasLowMemoryFallbackTriggered();
    lastLayoutAbortedForLowMemory_ = lastLayoutAbortedForLowMemory_ || build_->parser->wasLowMemoryAbortTriggered();
    if (build_->pageCompletionFailed || status == ChapterHtmlSlimParser::ParseStatus::Error) {
      LOG_ERR("SCT", "Failed during incremental section build");
      // A low-memory replay over an existing partial may fail before rebuilding page 0.
      // Suspend in that case too so suspendBuild() keeps the older readable partial.
      if (lastLayoutAbortedForLowMemory_ && (builtPageCount_ > 0 || partial_)) {
        suspendBuild();
      } else {
        abandonBuild();
      }
      return false;
    }
    if (status == ChapterHtmlSlimParser::ParseStatus::Done) {
      return finalizeBuild();
    }
    // ParseStatus::More: yield once we've laid out the requested number of pages.
    if (maxPages > 0 && (builtPageCount_ - startCount) >= maxPages) {
      build_->bytesConsumed = build_->parser->parseBytesConsumed();
      return true;
    }
  }
}

bool Section::hasHtmlCache() const {
  ReflowResource borrowedSection;
  if (document->getImmutableLocalSection(sectionIndex, borrowedSection)) {
    return Storage.exists(borrowedSection.localPath.c_str());
  }
  const std::string htmlPath = document->getCachePath() + "/html/" + std::to_string(sectionIndex) + ".html";
  return Storage.exists(htmlPath.c_str());
}

std::optional<uint16_t> Section::findAnchorDuringBuild(const std::string& anchor) const {
  if (!build_ || !build_->parser) return std::nullopt;
  for (const auto& [key, page] : build_->parser->getAnchors()) {
    if (key == anchor) return page;
  }
  return std::nullopt;
}

std::optional<uint16_t> Section::findAnchor(const std::string& anchor) const {
  if (const auto page = findAnchorDuringBuild(anchor)) {
    return page;
  }
  // Fall back to the on-disk anchor map: a finalized section, or a partial whose map
  // covers everything up to its watermark (nullopt past it -- build further and retry).
  return getPageForAnchor(anchor);
}

uint16_t Section::estimatedTotalPages() const {
  // Extrapolation from a suspended session's watermark trailer. A static snapshot, so no EMA
  // damping is needed. Also the best guess while a rebuild is running but hasn't laid out
  // enough pages yet to extrapolate from its own progress.
  const auto partialEstimate = [this]() -> uint16_t {
    if (!partial_ || partialBytesConsumed_ == 0 || partialTotalBytes_ <= partialBytesConsumed_) {
      return pageCount;
    }
    const uint32_t est = PageCountEstimator::estimate(partialPageCount_, partialProtectedImageUnits_,
                                                      partialTotalBytes_, partialBytesConsumed_);
    if (est <= pageCount) return pageCount;
    return static_cast<uint16_t>(est);
  };

  if (!build_) {
    return partial_ ? partialEstimate() : pageCount;  // partial -> extrapolate, finalized -> exact
  }
  const uint32_t consumed = build_->bytesConsumed;
  const uint32_t total = build_->totalBytes;
  if (builtPageCount_ == 0 || consumed == 0 || total <= consumed) return partialEstimate();

  // Raw extrapolation protects image units and scales only the non-image portion
  // by the fraction of XHTML still unparsed. This keeps image-heavy prefixes
  // from being multiplied by their byte size.
  const uint32_t raw = PageCountEstimator::estimate(builtPageCount_, protectedImageUnits_, total, consumed);

  // Damp that jitter with an exponential moving average. Step it once per build advance (keyed on
  // bytesConsumed) rather than per status-bar redraw, so the smoothing rate doesn't depend on how
  // often we repaint. As the build nears the end, consumed -> total and raw -> the built count, so
  // the average settles onto the true count (and finalizeBuild then returns the exact pageCount).
  constexpr float ALPHA = 0.25f;  // weight of each new sample; lower = steadier but slower to settle
  if (build_->smoothedEstimate <= 0) {
    build_->smoothedEstimate = static_cast<float>(raw);  // seed on the first estimate
  } else if (consumed != build_->smoothedAtConsumed) {
    build_->smoothedEstimate += ALPHA * (static_cast<float>(raw) - build_->smoothedEstimate);
  }
  build_->smoothedAtConsumed = consumed;

  const uint64_t est = static_cast<uint64_t>(build_->smoothedEstimate + 0.5f);
  if (est <= pageCount) return pageCount;  // never fewer than the pages already available
  return est > PageCountEstimator::kMaxPages ? PageCountEstimator::kMaxPages : static_cast<uint16_t>(est);
}

uint64_t Section::estimatedProtectedImageUnits() const {
  if (build_ && builtPageCount_ > 0) return protectedImageUnits_;
  if (partial_) return partialProtectedImageUnits_;
  return protectedImageUnits_;
}

uint64_t Section::estimatedNonImageProjectionUnits() const {
  const uint32_t physicalPages =
      build_ && builtPageCount_ > 0 ? builtPageCount_ : (partial_ ? partialPageCount_ : pageCount);
  const uint64_t imageUnits = estimatedProtectedImageUnits();
  const uint64_t knownNonImageUnits = PageCountEstimator::nonImageUnits(physicalPages, imageUnits);
  if (build_ && builtPageCount_ > 0) {
    return PageCountEstimator::projectNonImageUnits(knownNonImageUnits, build_->totalBytes, build_->bytesConsumed);
  }
  if (partial_) {
    return PageCountEstimator::projectNonImageUnits(knownNonImageUnits, partialTotalBytes_, partialBytesConsumed_);
  }
  return knownNonImageUnits;
}

// Write the LUTs and anchor map into the open tmp .bin, patch the header with the built
// page count and table offsets, stamp `version` as the commit point, then swap the tmp
// file over filePath. For SECTION_FILE_PARTIAL_VERSION a watermark trailer
// (bytesConsumed, totalBytes) is appended after the visible-text LUT so a later open can estimate
// the total page count. The parser must still be alive (anchors are read from it).
// On failure the tmp is removed and any pre-existing file at filePath is left intact.
bool Section::commitBuildFile(const uint8_t version, const uint32_t bytesConsumed, const uint32_t totalBytes) {
  const bool asPartial = (version == SECTION_FILE_PARTIAL_VERSION);

  const auto failCommit = [this]() {
    // Explicit close() required before remove (member variable, O_RDWR handle).
    file.close();
    if (build_ && !build_->tmpSectionPath.empty()) {
      Storage.remove(build_->tmpSectionPath.c_str());
    }
    return false;
  };

  if (!ensureBuildFileOpen()) {
    return failCommit();
  }

  const auto& anchors = build_->parser->getAnchors();
  SectionPageIndexOffsets pageIndexOffsets;
  const bool wrotePageIndex = writeSectionPageIndex(
      file, build_->pageIndex,
      [this, asPartial, &anchors](FsFile& output) {
        uint16_t anchorCount = 0;
        for (const auto& [anchor, page] : anchors) {
          if (!asPartial || page < builtPageCount_) anchorCount++;
        }
        if (!serialization::tryWritePod(output, anchorCount)) return false;
        for (const auto& [anchor, page] : anchors) {
          if (asPartial && page >= builtPageCount_) continue;
          if (!serialization::tryWriteString(output, anchor) || !serialization::tryWritePod(output, page)) return false;
        }
        return true;
      },
      pageIndexOffsets);
  if (!wrotePageIndex) {
    LOG_ERR("SCT", "Failed to write section page index");
    return failCommit();
  }

  if (asPartial) {
    // Watermark trailer follows the page-start visible-text LUT.
    if (!serialization::tryWritePod(file, bytesConsumed) || !serialization::tryWritePod(file, totalBytes)) {
      return failCommit();
    }
  }

  // Patch header with the built page count and section offsets...
  if (!file.seek(HEADER_SIZE - sizeof(uint32_t) * 6 - sizeof(builtPageCount_)) ||
      !serialization::tryWritePod(file, builtPageCount_) || !serialization::tryWritePod(file, protectedImageUnits_) ||
      !serialization::tryWritePod(file, pageIndexOffsets.page) ||
      !serialization::tryWritePod(file, pageIndexOffsets.anchorMap) ||
      !serialization::tryWritePod(file, pageIndexOffsets.paragraph) ||
      !serialization::tryWritePod(file, pageIndexOffsets.listItem) ||
      !serialization::tryWritePod(file, pageIndexOffsets.visibleText) || !file.seek(sizeof(SECTION_CACHE_MAGIC)) ||
      !serialization::tryWritePod(file, version) || !file.sync()) {
    LOG_ERR("SCT", "Failed to commit section cache");
    return failCommit();
  }
  // Explicit close() required: member variable persists beyond function scope
  file.close();

  // Keep the readable cache as a backup until the completed replacement is in
  // place. A reboot after the backup rename recovers it in the constructor.
  if (!promoteSectionCache(build_->tmpSectionPath, filePath)) {
    LOG_ERR("SCT", "Failed to move built section into place");
    Storage.remove(build_->tmpSectionPath.c_str());
    return false;
  }
  return true;
}

bool Section::finalizeBuild() {
  if (!build_ || !build_->parser) {
    return false;
  }

  const bool success = build_->parser->finishParse();
  lastImagesWereSuppressed_ = lastImagesWereSuppressed_ || build_->parser->wasLowMemoryFallbackTriggered();
  lastLayoutAbortedForLowMemory_ = lastLayoutAbortedForLowMemory_ || build_->parser->wasLowMemoryAbortTriggered();
  if (!success || build_->pageCompletionFailed) {
    LOG_ERR("SCT", "Failed to finalize parser output");
    abandonBuild();
    return false;
  }

  const bool committed = commitBuildFile(SECTION_FILE_VERSION, 0, 0);
  if (build_->cssParser) build_->cssParser->clear();
  if (!build_->reusedHtml && Storage.exists(build_->tmpHtmlPath.c_str())) {
    Storage.remove(build_->tmpHtmlPath.c_str());
  }
  build_.reset();
  if (!committed) {
    // The previous cache was retained/restored by promoteSectionCache(). Keep
    // its in-memory watermark too so this Section remains readable.
    pageCount = partial_ ? partialPageCount_ : 0;
    builtPageCount_ = 0;
    return false;
  }
  buildComplete_ = true;
  partial_ = false;
  partialPageCount_ = 0;
  partialProtectedImageUnits_ = 0;
  pageCount = builtPageCount_;
  return true;
}

void Section::suspendBuild() {
  if (!build_) return;

  // Only worth persisting if this build produced pages a pre-existing partial doesn't
  // already cover; otherwise keep the older (bigger) partial and just drop the tmp.
  const bool worthKeeping = builtPageCount_ > 0 && (!partial_ || builtPageCount_ > partialPageCount_);

  bool committed = false;
  if (worthKeeping) {
    // Capture the parse watermark and commit before tearing the parser down (the anchor
    // map is read from it). The incomplete trailing page is intentionally not flushed:
    // only fully laid-out pages are persisted, and the rebuild re-derives the rest.
    const uint32_t consumed = static_cast<uint32_t>(build_->parser->parseBytesConsumed());
    committed = commitBuildFile(SECTION_FILE_PARTIAL_VERSION, consumed, build_->totalBytes);
    if (committed) {
      partial_ = true;
      partialPageCount_ = builtPageCount_;
      partialProtectedImageUnits_ = protectedImageUnits_;
      partialBytesConsumed_ = consumed;
      partialTotalBytes_ = build_->totalBytes;
      LOG_INF("SCT", "Suspended build: %u pages persisted", builtPageCount_);
    }
  }

  if (build_->parser) build_->parser->abortParse();
  if (build_->cssParser) build_->cssParser->clear();
  if (!committed && file) {
    // Explicit close() required before remove (member variable, O_RDWR handle).
    file.close();
  }
  if (!committed && Storage.exists(binTmpPath().c_str())) {
    Storage.remove(binTmpPath().c_str());
  }
  if (!build_->reusedHtml && Storage.exists(build_->tmpHtmlPath.c_str())) {
    Storage.remove(build_->tmpHtmlPath.c_str());
  }
  build_.reset();
  buildComplete_ = false;
  pageCount = partial_ ? partialPageCount_ : 0;
  builtPageCount_ = 0;
}

void Section::abandonBuild() {
  if (!build_) return;
  if (build_->parser) {
    build_->parser->abortParse();
  }
  if (build_->cssParser) {
    build_->cssParser->clear();
  }
  if (file) {
    file.close();
  }
  if (!build_->tmpSectionPath.empty() && Storage.exists(build_->tmpSectionPath.c_str())) {
    Storage.remove(build_->tmpSectionPath.c_str());
  }
  // A parse error would recur against the same HTML, so drop any partial too -- resuming
  // from it would just re-enter the failing build every open.
  if (Storage.exists(filePath.c_str())) {
    Storage.remove(filePath.c_str());
  }
  if (!build_->reusedHtml && Storage.exists(build_->tmpHtmlPath.c_str())) {
    Storage.remove(build_->tmpHtmlPath.c_str());
  }
  build_.reset();
  buildComplete_ = false;
  partial_ = false;
  partialPageCount_ = 0;
  partialProtectedImageUnits_ = 0;
  protectedImageUnits_ = 0;
  pageCount = 0;
  builtPageCount_ = 0;
}

std::unique_ptr<Page> Section::loadPageDuringBuild(const int page) {
  if (!build_ || page < 0 || static_cast<size_t>(page) >= build_->pageIndex.size()) {
    return nullptr;
  }
  const uint32_t pos = build_->pageIndex[static_cast<size_t>(page)].fileOffset;
  if (pos == 0) {
    return nullptr;
  }

  if (!file) {
    HalFile tmp;
    if (!Storage.openFileForRead("SCT", build_->tmpSectionPath, tmp) || !tmp.seek(pos)) {
      return nullptr;
    }
    return Page::deserialize(tmp);
  }

  const uint32_t writePos = file.position();
  if (!file.seek(pos)) {
    return nullptr;
  }
  auto pageData = Page::deserialize(file);
  file.seek(writePos);
  return pageData;
}

// Read a page from the committed file at filePath (finalized section or partial from a
// previous session). Uses a local handle so it is safe while a build holds the member
// `file` open on the tmp .bin.
std::unique_ptr<Page> Section::loadPageAt(const int page) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return nullptr;
  }

  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 5)) {
    return nullptr;
  }
  uint32_t lutOffset;
  if (!serialization::tryReadPod(f, lutOffset) || !f.seek(lutOffset + sizeof(uint32_t) * page)) {
    return nullptr;
  }
  uint32_t pagePos;
  if (!serialization::tryReadPod(f, pagePos) || !f.seek(pagePos)) {
    return nullptr;
  }

  return Page::deserialize(f);
  // No f.close() needed -- DESTRUCTOR_CLOSES_FILE=1 handles it at scope exit
}

std::unique_ptr<Page> Section::loadPage(const int page) {
  if (page < 0) {
    return nullptr;
  }
  if (build_ && static_cast<size_t>(page) < build_->pageIndex.size()) {
    return loadPageDuringBuild(page);
  }
  // Not (yet) in the active build: serve from the file on disk -- a finalized section,
  // or a partial from a previous session whose pages the rebuild hasn't reached again.
  const int onDisk = partial_ ? partialPageCount_ : (build_ ? 0 : pageCount);
  if (page >= onDisk) {
    return nullptr;
  }
  return loadPageAt(page);
}

std::unique_ptr<Page> Section::loadPageFromSectionFile() { return loadPage(currentPage); }

std::optional<uint16_t> Section::getCachedPageCount() const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (fileSize < HEADER_SIZE) {
    return std::nullopt;
  }

  // Only a finalized section's count is the chapter total; a partial's count is just the
  // suspended build's watermark, which would skew progress mapping. Callers fall back to
  // their own estimates.
  uint32_t magic;
  if (!serialization::tryReadPod(f, magic) || magic != SECTION_CACHE_MAGIC) {
    return std::nullopt;
  }
  uint8_t version;
  if (!serialization::tryReadPod(f, version)) {
    return std::nullopt;
  }
  if (version != SECTION_FILE_VERSION) {
    return std::nullopt;
  }

  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 6 - sizeof(uint16_t))) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(f, count)) {
    return std::nullopt;
  }
  return count;
}

std::optional<uint16_t> Section::getPageForAnchor(const std::string& anchor) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 4)) {
    return std::nullopt;
  }
  uint32_t anchorMapOffset;
  if (!serialization::tryReadPod(f, anchorMapOffset)) {
    return std::nullopt;
  }
  if (anchorMapOffset == 0 || anchorMapOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek(anchorMapOffset)) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(f, count)) {
    return std::nullopt;
  }
  for (uint16_t i = 0; i < count; i++) {
    std::string key;
    uint16_t page;
    if (!serialization::tryReadString(f, key) || !serialization::tryReadPod(f, page)) {
      return std::nullopt;
    }
    if (key == anchor) {
      return page;
    }
  }

  return std::nullopt;
}

std::optional<uint16_t> Section::getPageForParagraphIndex(const uint16_t pIndex) const {
  if (const auto page = findParagraphDuringBuild(pIndex)) {
    return page;
  }

  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 3)) {
    return std::nullopt;
  }
  uint32_t paragraphLutOffset;
  if (!serialization::tryReadPod(f, paragraphLutOffset)) {
    return std::nullopt;
  }
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek(paragraphLutOffset)) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(f, count)) {
    return std::nullopt;
  }
  if (count == 0) {
    return std::nullopt;
  }

  const uint32_t lutEnd = paragraphLutOffset + sizeof(uint16_t) + count * sizeof(uint16_t);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pagePIdx;
    if (!serialization::tryReadPod(f, pagePIdx)) {
      return std::nullopt;
    }
    if (pagePIdx >= pIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}

std::optional<uint16_t> Section::findParagraphDuringBuild(const uint16_t pIndex) const {
  if (build_) {
    for (size_t i = 0; i < build_->pageIndex.size(); i++) {
      if (build_->pageIndex[i].paragraphIndex >= pIndex) {
        return static_cast<uint16_t>(i);
      }
    }
  }
  return std::nullopt;
}

std::optional<uint16_t> Section::getParagraphIndexForPage(const uint16_t page) const {
  if (build_ && page < build_->pageIndex.size()) {
    return build_->pageIndex[page].paragraphIndex;
  }

  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 3)) {
    return std::nullopt;
  }
  uint32_t paragraphLutOffset;
  if (!serialization::tryReadPod(f, paragraphLutOffset)) {
    return std::nullopt;
  }
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek(paragraphLutOffset)) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(f, count)) {
    return std::nullopt;
  }
  if (count == 0 || page >= count) {
    return std::nullopt;
  }

  const uint32_t entryEnd = paragraphLutOffset + sizeof(uint16_t) + (page + 1) * sizeof(uint16_t);
  if (entryEnd > fileSize) {
    return std::nullopt;
  }

  if (!f.seek(paragraphLutOffset + sizeof(uint16_t) + page * sizeof(uint16_t))) {
    return std::nullopt;
  }
  uint16_t pIdx;
  if (!serialization::tryReadPod(f, pIdx)) {
    return std::nullopt;
  }
  return pIdx;
}

std::optional<uint16_t> Section::getListItemIndexForPage(const uint16_t page) const {
  if (build_ && page < build_->pageIndex.size()) {
    return build_->pageIndex[page].listItemIndex;
  }

  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 2)) {
    return std::nullopt;
  }
  uint32_t liLutOffset;
  if (!serialization::tryReadPod(f, liLutOffset)) {
    return std::nullopt;
  }
  if (liLutOffset == 0 || liLutOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 3)) {
    return std::nullopt;
  }
  uint32_t paragraphLutOffset;
  if (!serialization::tryReadPod(f, paragraphLutOffset)) {
    return std::nullopt;
  }
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek(paragraphLutOffset)) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(f, count)) {
    return std::nullopt;
  }
  if (count == 0 || page >= count) {
    return std::nullopt;
  }

  const uint32_t entryEnd = liLutOffset + (page + 1) * sizeof(uint16_t);
  if (entryEnd > fileSize) {
    return std::nullopt;
  }

  if (!f.seek(liLutOffset + page * sizeof(uint16_t))) {
    return std::nullopt;
  }
  uint16_t liIdx;
  if (!serialization::tryReadPod(f, liIdx)) {
    return std::nullopt;
  }
  return liIdx;
}

std::optional<uint16_t> Section::getPageForListItemIndex(const uint16_t liIndex) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 2)) {
    return std::nullopt;
  }
  uint32_t liLutOffset;
  if (!serialization::tryReadPod(f, liLutOffset)) {
    return std::nullopt;
  }
  if (liLutOffset == 0 || liLutOffset >= fileSize) {
    return std::nullopt;
  }

  // The li LUT shares count with the paragraph LUT; read count from paragraphLutOffset
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 3)) {
    return std::nullopt;
  }
  uint32_t paragraphLutOffset;
  if (!serialization::tryReadPod(f, paragraphLutOffset)) {
    return std::nullopt;
  }
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek(paragraphLutOffset)) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(f, count)) {
    return std::nullopt;
  }
  if (count == 0) {
    return std::nullopt;
  }

  const uint32_t lutEnd = liLutOffset + count * sizeof(uint16_t);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  if (!f.seek(liLutOffset)) {
    return std::nullopt;
  }
  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pageLiIdx;
    if (!serialization::tryReadPod(f, pageLiIdx)) {
      return std::nullopt;
    }
    if (pageLiIdx >= liIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}

std::optional<uint32_t> Section::getVisibleTextOffsetForPage(const uint16_t page) const {
  if (build_ && page < build_->pageIndex.size()) {
    return build_->pageIndex[page].visibleTextOffset;
  }

  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) return std::nullopt;
  const uint32_t fileSize = f.size();
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 6 - sizeof(uint16_t))) return std::nullopt;
  uint16_t count = 0;
  if (!serialization::tryReadPod(f, count) || page >= count) return std::nullopt;
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t))) return std::nullopt;
  uint32_t lutOffset = 0;
  if (!serialization::tryReadPod(f, lutOffset) || lutOffset < HEADER_SIZE ||
      lutOffset + (static_cast<uint32_t>(page) + 1U) * sizeof(uint32_t) > fileSize ||
      !f.seek(lutOffset + static_cast<uint32_t>(page) * sizeof(uint32_t))) {
    return std::nullopt;
  }
  uint32_t offset = 0;
  return serialization::tryReadPod(f, offset) ? std::optional<uint32_t>(offset) : std::nullopt;
}

std::optional<uint16_t> Section::getPageForVisibleTextOffset(const uint32_t offset,
                                                             const bool preferFirstAtOffset) const {
  if (build_ && !build_->pageIndex.empty()) {
    uint16_t result = 0;
    for (size_t i = 0; i < build_->pageIndex.size(); i++) {
      const uint32_t start = build_->pageIndex[i].visibleTextOffset;
      if (start > offset) break;
      result = static_cast<uint16_t>(i);
      if (preferFirstAtOffset && start == offset) break;
    }
    const uint32_t last = build_->pageIndex[build_->pageIndex.size() - 1].visibleTextOffset;
    if (offset <= last) return result;
    // An extension build starts from page zero while its previously committed
    // partial remains readable.  Its shorter live prefix must not hide a page
    // that the committed cache already knows how to resolve.
  }

  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) return std::nullopt;
  const uint32_t fileSize = f.size();
  uint32_t magic = 0;
  uint8_t version = 0;
  if (!serialization::tryReadPod(f, magic) || magic != SECTION_CACHE_MAGIC || !serialization::tryReadPod(f, version) ||
      (version != SECTION_FILE_VERSION && version != SECTION_FILE_PARTIAL_VERSION) ||
      !f.seek(HEADER_SIZE - sizeof(uint32_t) * 6 - sizeof(uint16_t))) {
    return std::nullopt;
  }
  uint16_t count = 0;
  if (!serialization::tryReadPod(f, count) || count == 0 || !f.seek(HEADER_SIZE - sizeof(uint32_t))) {
    return std::nullopt;
  }
  uint32_t lutOffset = 0;
  if (!serialization::tryReadPod(f, lutOffset) || lutOffset < HEADER_SIZE ||
      lutOffset + static_cast<uint32_t>(count) * sizeof(uint32_t) > fileSize) {
    return std::nullopt;
  }
  uint16_t result = 0;
  uint32_t last = 0;
  for (uint16_t i = 0; i < count; i++) {
    uint32_t start = 0;
    if (!f.seek(lutOffset + static_cast<uint32_t>(i) * sizeof(uint32_t)) || !serialization::tryReadPod(f, start)) {
      return std::nullopt;
    }
    last = start;
    if (start > offset) break;
    result = i;
    if (preferFirstAtOffset && start == offset) break;
  }
  if (version == SECTION_FILE_PARTIAL_VERSION && offset > last) return std::nullopt;
  return result;
}

std::optional<ReflowPageSemanticRange> Section::getSemanticRangeForPage(const uint16_t page) {
  if (!usesPdfWordIndex() || page >= pageCount) {
    return std::nullopt;
  }
  file.close();
  ReflowPageSemanticRange range;
  return document->readLayoutWordRange(filePath, pageCount, page, range) ? std::optional<ReflowPageSemanticRange>(range)
                                                                         : std::nullopt;
}

std::optional<uint16_t> Section::getPageForSemanticPosition(const char* const blockAnchor,
                                                            const uint32_t blockWordOffset,
                                                            const uint32_t globalWordOrdinal) {
  if (!usesPdfWordIndex()) {
    return std::nullopt;
  }
  file.close();
  uint16_t page = 0;
  return document->findLayoutWordPage(filePath, blockAnchor, blockWordOffset, globalWordOrdinal, page)
             ? std::optional<uint16_t>(page)
             : std::nullopt;
}

std::optional<uint16_t> Section::getPageForSemanticCursor(const uint32_t wordCursor) {
  if (!usesPdfWordIndex()) {
    return std::nullopt;
  }
  file.close();
  uint16_t page = 0;
  return document->findLayoutWordCursor(filePath, wordCursor, page) ? std::optional<uint16_t>(page) : std::nullopt;
}
