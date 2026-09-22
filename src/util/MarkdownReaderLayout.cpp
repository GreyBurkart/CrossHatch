#include "MarkdownReaderLayout.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Txt.h>

#include <algorithm>
#include <cstring>
#include <string_view>

#include "MarkdownParser.h"

namespace {
constexpr size_t BLOCK_BYTES = 2048;
constexpr size_t PAGE_BYTES = 4096;
constexpr size_t PAGE_RUNS = 192;
constexpr uint32_t MAGIC = 0x31444d43;  // CMD1, ephemeral layout version 1
constexpr uint8_t CODE = 4;
constexpr uint8_t QUOTE = 8;
constexpr uint8_t RULE = 16;
struct Run {
  int32_t font;
  uint16_t offset;
  uint16_t length;
  int16_t x;
  int16_t y;
  int16_t width;
  uint8_t flags;
  uint8_t reserved;
};
static_assert(sizeof(Run) == 16, "Markdown spool run layout changed: bump format version");
struct IndexEntry {
  uint32_t position;
  uint32_t source;
};

// Split very long physical lines without splitting a UTF-8 sequence. Such chunks
// have bounded literal/inline parsing; no source bytes are dropped at the limit.
bool readLine(HalFile& file, char* line, size_t& length, bool& continues, const size_t fileSize) {
  length = 0;
  continues = false;
  while (length < BLOCK_BYTES && file.position() < fileSize) {
    const int c = file.read();
    if (c < 0) return false;
    if (c == '\n') break;
    line[length++] = static_cast<char>(c);
  }
  if (length == BLOCK_BYTES && file.position() < fileSize) {
    const size_t end = file.position();
    const int next = file.read();
    if (next < 0) return false;
    if (next != '\n') {
      size_t cut = length;
      if ((next & 0xc0) == 0x80) {
        while (cut > 0 && (static_cast<uint8_t>(line[cut - 1]) & 0xc0) == 0x80) --cut;
        if (cut > 0) --cut;
        // Malformed input still makes bounded forward progress.
        if (cut == 0) cut = length;
      }
      if (!file.seek(end - (length - cut))) return false;
      length = cut;
      continues = true;
    }
  }
  if (!continues && length && line[length - 1] == '\r') --length;
  line[length] = '\0';
  return true;
}
size_t characterBytes(const char* text, size_t remaining) {
  size_t n = 1;
  while (n < remaining && n < 4 && (static_cast<uint8_t>(text[n]) & 0xc0) == 0x80) ++n;
  return n;
}
EpdFontFamily::Style fontStyle(uint8_t flags) { return static_cast<EpdFontFamily::Style>(flags & 3); }
bool hasHardBreak(std::string_view line) {
  if (line.size() >= 2 && line[line.size() - 1] == ' ' && line[line.size() - 2] == ' ') return true;
  size_t backslashes = 0;
  while (backslashes < line.size() && line[line.size() - backslashes - 1] == '\\') ++backslashes;
  return (backslashes & 1) != 0;
}
}  // namespace

struct MarkdownReaderLayout::Workspace {
  char line[BLOCK_BYTES + 1];
  char block[BLOCK_BYTES + 1];
  char text[BLOCK_BYTES + 1];
  uint8_t styles[BLOCK_BYTES + 1];
  Run runs[PAGE_RUNS];
  char pageText[PAGE_BYTES];
  uint16_t runCount = 0;
  uint16_t textCount = 0;
};

// ~15 KiB allocated once at activity entry (or a one-shot sleep snapshot).
// Too large for a task stack; unique ownership releases it when reading ends.
MarkdownReaderLayout::MarkdownReaderLayout() : work(makeUniqueNoThrow<Workspace>()) {}
MarkdownReaderLayout::~MarkdownReaderLayout() = default;

bool MarkdownReaderLayout::build(const Txt& txt, GfxRenderer& renderer, const Config& config) {
  pages = 0;
  if (!work) {
    LOG_ERR("MD", "Could not allocate %zu-byte layout workspace", sizeof(Workspace));
    return false;
  }
  dataPath = txt.getCachePath() + "/markdown.pages";
  indexPath = txt.getCachePath() + "/markdown.index";
  HalFile source, data, index;
  bool ok = Storage.openFileForRead("MD", txt.getPath(), source) && Storage.openFileForWrite("MD", dataPath, data) &&
            Storage.openFileForWrite("MD", indexPath, index);
  if (!ok) {
    source.close();
    data.close();
    index.close();
    Storage.remove(dataPath.c_str());
    Storage.remove(indexPath.c_str());
    LOG_ERR("MD", "Could not open layout files");
    return false;
  }
  ok = data.write(&MAGIC, sizeof(MAGIC)) == sizeof(MAGIC) && index.write(&MAGIC, sizeof(MAGIC)) == sizeof(MAGIC);
  auto& w = *work;
  w.runCount = w.textCount = 0;
  const int width = std::max(1, config.width);
  const int height = std::max(1, config.height);
  const int bodyHeight = std::max(1, config.lineHeight);
  int y = 0;
  uint32_t pageSource = 0;
  auto flushPage = [&]() {
    if (!ok || w.runCount == 0) return;
    if (pages >= 65535) {
      LOG_ERR("MD", "Document exceeds page limit");
      ok = false;
      return;
    }
    const IndexEntry entry{static_cast<uint32_t>(data.position()), pageSource};
    ok = index.write(&entry, sizeof(entry)) == sizeof(entry) &&
         data.write(&w.runCount, sizeof(w.runCount)) == sizeof(w.runCount) &&
         data.write(&w.textCount, sizeof(w.textCount)) == sizeof(w.textCount) &&
         data.write(w.runs, w.runCount * sizeof(Run)) == w.runCount * sizeof(Run) &&
         data.write(w.pageText, w.textCount) == w.textCount;
    if (ok) ++pages;
    w.runCount = w.textCount = 0;
    y = 0;
  };
  auto ensureLine = [&](int lineHeight) {
    if (y + lineHeight > height && w.runCount) flushPage();
    if (!w.runCount) y = 0;
  };

  MarkdownParser::FenceState fence{};
  MarkdownParser::Block previous{};
  bool continuation = false;
  char previousMarker[24] = {};
  while (ok && source.position() < txt.getFileSize()) {
    const uint32_t blockSource = source.position();
    size_t length = 0;
    bool continues = false;
    if (!readLine(source, w.line, length, continues, txt.getFileSize())) {
      ok = false;
      break;
    }
    bool hardBreak = !continues && hasHardBreak(std::string_view(w.line, length));
    const bool trailingBreakSlash = hardBreak && length && w.line[length - 1] == '\\';
    auto block = continuation ? previous : MarkdownParser::classifyLine(std::string_view(w.line, length), fence);
    if (continuation) block.content = std::string_view(w.line, length);
    char marker[24] = {};
    if (continuation) {
      std::memcpy(marker, previousMarker, sizeof(marker));
    } else if (!block.marker.empty()) {
      const size_t n = std::min(block.marker.size(), sizeof(marker) - 2);
      std::memcpy(marker, block.marker.data(), n);
      marker[n] = ' ';
    }
    previous = block;
    std::memcpy(previousMarker, marker, sizeof(marker));
    const bool wasContinuation = continuation;
    continuation = continues;
    using Kind = MarkdownParser::BlockKind;
    if (block.kind == Kind::Fence) continue;
    if (block.kind == Kind::Blank) {
      if (w.runCount) y += std::max(1, bodyHeight / 2);
      continue;
    }
    if (block.kind == Kind::Rule) {
      ensureLine(bodyHeight);
      if (w.runCount == PAGE_RUNS) flushPage();
      if (!w.runCount) pageSource = blockSource;
      w.runs[w.runCount++] = {
          config.fontId, 0, 0, 0, static_cast<int16_t>(y + bodyHeight / 2), static_cast<int16_t>(width), RULE, 0};
      y += bodyHeight;
      continue;
    }

    size_t blockLength = block.content.size();
    std::memcpy(w.block, block.content.data(), blockLength);
    if ((block.kind == Kind::Paragraph || block.kind == Kind::Quote || block.kind == Kind::ListItem) &&
        trailingBreakSlash && blockLength)
      --blockLength;
    // Ordinary physical newlines are soft breaks. Lists accept indented prose
    // continuation lines; new list items and other blocks keep their structure.
    while (!continuation && !hardBreak && source.position() < txt.getFileSize() &&
           (block.kind == Kind::Paragraph || block.kind == Kind::Quote || block.kind == Kind::ListItem)) {
      const size_t nextPosition = source.position();
      size_t nextLength = 0;
      bool nextContinues = false;
      auto nextFence = fence;
      if (!readLine(source, w.line, nextLength, nextContinues, txt.getFileSize())) {
        ok = false;
        break;
      }
      const bool nextHardBreak = !nextContinues && hasHardBreak(std::string_view(w.line, nextLength));
      const bool nextBreakSlash = nextHardBreak && nextLength && w.line[nextLength - 1] == '\\';
      const auto next = MarkdownParser::classifyLine(std::string_view(w.line, nextLength), nextFence);
      const bool compatible =
          (block.kind == Kind::Paragraph && next.kind == Kind::Paragraph) ||
          (block.kind == Kind::Quote && next.kind == Kind::Quote) ||
          (block.kind == Kind::ListItem && next.kind == Kind::Paragraph && next.indent > block.indent);
      if (!compatible || blockLength + next.content.size() + 1 > BLOCK_BYTES) {
        if (!source.seek(nextPosition)) ok = false;
        break;
      }
      w.block[blockLength++] = ' ';
      std::memcpy(w.block + blockLength, next.content.data(), next.content.size());
      blockLength += next.content.size();
      if (nextBreakSlash && blockLength) --blockLength;
      hardBreak = nextHardBreak;
      fence = nextFence;
      continuation = nextContinues;
    }
    if (!ok) break;
    w.block[blockLength] = '\0';
    size_t textLength = 0;
    if (block.kind == Kind::Code) {
      std::memcpy(w.text, w.block, blockLength + 1);
      std::memset(w.styles, CODE, blockLength);
      textLength = blockLength;
    } else if (!MarkdownParser::parseInline(std::string_view(w.block, blockLength), w.text, w.styles, sizeof(w.text),
                                            textLength)) {
      ok = false;
      break;
    }
    const bool heading = block.kind == Kind::Heading;
    const int font = heading && block.headingLevel <= 2 ? config.headingFontId : config.fontId;
    const int lineHeight = heading ? std::max(bodyHeight, renderer.getLineHeight(font)) : bodyHeight;
    // Code already retains its source indentation in text; add only its gutter.
    const int sourceIndent = block.kind == Kind::Code ? 0 : static_cast<int>(block.indent) * bodyHeight / 2;
    const int indent = std::min(
        width / 3,
        sourceIndent +
            ((block.kind == Kind::ListItem || block.kind == Kind::Quote || block.kind == Kind::Code) ? bodyHeight : 0));
    const uint8_t baseFlags = (heading ? 1 : 0) | (block.kind == Kind::Quote ? QUOTE : 0);
    if (heading && w.runCount && !wasContinuation) y += bodyHeight / 2;
    ensureLine(lineHeight);
    if (renderer.isSdCardFont(font)) renderer.ensureSdCardFontReady(font, w.text, 0x0f);
    int x = indent;
    const int right = std::max(indent + 1, width - (block.kind == Kind::Code ? bodyHeight / 2 : 0));
    auto canMerge = [&](uint8_t flags) {
      return w.runCount && !(w.runs[w.runCount - 1].flags & RULE) && w.runs[w.runCount - 1].flags == flags &&
             w.runs[w.runCount - 1].font == font && w.runs[w.runCount - 1].y == y &&
             w.runs[w.runCount - 1].x + w.runs[w.runCount - 1].width == x;
    };
    auto measureAddition = [&](const char* bytes, size_t n, uint8_t flags) {
      if (!canMerge(flags) || w.textCount + n + 1 >= PAGE_BYTES) {
        return renderer.getTextAdvanceX(font, bytes, fontStyle(flags));
      }
      // Measure exactly the complete string drawText will receive, including
      // kerning and ligatures across appended characters. Reuse the page buffer
      // for this bounded lookahead instead of allocating a temporary string.
      auto& run = w.runs[w.runCount - 1];
      std::memcpy(w.pageText + w.textCount - 1, bytes, n);
      w.pageText[w.textCount + n - 1] = '\0';
      const int advance = renderer.getTextAdvanceX(font, w.pageText + run.offset, fontStyle(flags)) - run.width;
      w.pageText[w.textCount - 1] = '\0';
      return advance;
    };
    auto append = [&](const char* bytes, size_t n, uint8_t flags, uint32_t anchor) {
      bool merge = canMerge(flags);
      if (w.textCount + n + 1 >= PAGE_BYTES || (!merge && w.runCount == PAGE_RUNS)) {
        flushPage();
        merge = false;
        x = indent;
      }
      if (!ok) return;
      const int advance = measureAddition(bytes, n, flags);
      if (!w.runCount) pageSource = anchor;
      if (!merge) {
        w.runs[w.runCount++] = {font, w.textCount, 0, static_cast<int16_t>(x), static_cast<int16_t>(y), 0, flags, 0};
        w.pageText[w.textCount++] = '\0';
      }
      auto& run = w.runs[w.runCount - 1];
      std::memcpy(w.pageText + w.textCount - 1, bytes, n);
      w.textCount += n;
      w.pageText[w.textCount - 1] = '\0';
      run.length += n;
      run.width += advance;
      x += advance;
    };
    if (block.kind == Kind::ListItem && !wasContinuation && marker[0]) {
      // Draw the list marker in the gutter; wrapped lines start at indent.
      const int markerWidth = renderer.getTextAdvanceX(font, marker, EpdFontFamily::REGULAR);
      x = std::max(0, indent - markerWidth);
      append(marker, std::strlen(marker), baseFlags, blockSource);
      x = std::max(indent, x);
    }
    // An empty code line still occupies a physical line, including at the top
    // of a page; an empty run prevents ensureLine from discarding that space.
    if (block.kind == Kind::Code && textLength == 0) append("", 0, CODE, blockSource);
    size_t pos = 0;
    while (ok && pos < textLength) {
      const bool space = w.text[pos] == ' ' || w.text[pos] == '\t';
      size_t wordEnd = pos + 1;
      if (!space)
        while (wordEnd < textLength && w.text[wordEnd] != ' ' && w.text[wordEnd] != '\t') ++wordEnd;
      // Word measurement uses the same font style and advance API as drawing.
      int wordWidth = 0;
      for (size_t at = pos; at < wordEnd;) {
        const uint8_t style = (w.styles[at] | baseFlags) & 3;
        size_t end = at + 1;
        while (end < wordEnd && ((w.styles[end] | baseFlags) & 3) == style) ++end;
        const char saved = w.text[end];
        w.text[end] = '\0';
        wordWidth += renderer.getTextAdvanceX(font, w.text + at, fontStyle(style));
        w.text[end] = saved;
        at = end;
      }
      if (!space && x > indent && x + wordWidth > right) {
        y += lineHeight;
        ensureLine(lineHeight);
        x = indent;
      }
      while (ok && pos < wordEnd) {
        const size_t n = characterBytes(w.text + pos, wordEnd - pos);
        char glyph[5] = {};
        std::memcpy(glyph, w.text + pos, n);
        const uint8_t flags = w.styles[pos] | baseFlags;
        int repeats = glyph[0] == '\t' ? 4 : 1;
        if (glyph[0] == '\t') glyph[0] = ' ';
        for (int repeat = 0; repeat < repeats && ok; ++repeat) {
          const int advance = measureAddition(glyph, n, flags);
          if (x > indent && x + advance > right) {
            y += lineHeight;
            ensureLine(lineHeight);
            x = indent;
          }
          // Collapse prose indentation at wraps, but preserve code whitespace.
          if (!(space && x == indent && block.kind != Kind::Code)) {
            append(glyph, n, flags, blockSource + static_cast<uint32_t>(pos));
          }
        }
        pos += n;
      }
    }
    y += lineHeight;
    if (heading) y += std::max(1, bodyHeight / (block.headingLevel <= 2 ? 3 : 5));
    delay(0);
  }
  flushPage();
  ok = ok && data.sync() && index.sync();
  source.close();
  data.close();
  index.close();
  if (!ok) {
    pages = 0;
    w.runCount = w.textCount = 0;
    Storage.remove(dataPath.c_str());
    Storage.remove(indexPath.c_str());
    LOG_ERR("MD", "Failed to build Markdown layout");
  }
  return ok;
}

bool MarkdownReaderLayout::loadPage(int page) {
  if (!work || page < 0 || page >= pages) return false;
  auto& w = *work;
  w.runCount = w.textCount = 0;
  HalFile index, data;
  IndexEntry entry{};
  bool ok = Storage.openFileForRead("MD", indexPath, index);
  if (ok) ok = index.seek(sizeof(MAGIC) + page * sizeof(entry)) && index.read(&entry, sizeof(entry)) == sizeof(entry);
  index.close();
  if (ok) ok = Storage.openFileForRead("MD", dataPath, data);
  if (ok)
    ok = data.seek(entry.position) && data.read(&w.runCount, 2) == 2 && data.read(&w.textCount, 2) == 2 &&
         w.runCount <= PAGE_RUNS && w.textCount <= PAGE_BYTES;
  if (ok)
    ok = data.read(w.runs, w.runCount * sizeof(Run)) == static_cast<int>(w.runCount * sizeof(Run)) &&
         data.read(w.pageText, w.textCount) == w.textCount;
  data.close();
  if (ok) {
    for (size_t i = 0; i < w.runCount; ++i) {
      const auto& run = w.runs[i];
      if (!(run.flags & RULE) &&
          (run.offset + run.length >= w.textCount || w.pageText[run.offset + run.length] != '\0')) {
        ok = false;
        break;
      }
    }
  }
  if (!ok) {
    w.runCount = w.textCount = 0;
    LOG_ERR("MD", "Could not read Markdown page %d", page);
  }
  return ok;
}

bool MarkdownReaderLayout::sourceOffset(int page, uint32_t& offset) const {
  if (page < 0 || page >= pages) {
    LOG_ERR("MD", "Invalid Markdown page position %d", page);
    return false;
  }
  HalFile index;
  IndexEntry entry{};
  const bool ok = Storage.openFileForRead("MD", indexPath, index) && index.seek(sizeof(MAGIC) + page * sizeof(entry)) &&
                  index.read(&entry, sizeof(entry)) == sizeof(entry);
  index.close();
  if (!ok) {
    LOG_ERR("MD", "Could not read Markdown page position");
    return false;
  }
  offset = entry.source;
  return true;
}

void MarkdownReaderLayout::draw(GfxRenderer& renderer, int left, int top, bool black) const {
  if (!work) return;
  for (size_t i = 0; i < work->runCount; ++i) {
    const auto& run = work->runs[i];
    if (run.flags & RULE) {
      if (renderer.getRenderMode() == GfxRenderer::BW)
        renderer.drawLine(left, top + run.y, left + run.width - 1, top + run.y, black);
      continue;
    }
    if (renderer.getRenderMode() == GfxRenderer::BW) {
      if (run.flags & QUOTE)
        renderer.drawLine(left + 2, top + run.y, left + 2, top + run.y + renderer.getLineHeight(run.font) - 1, black);
      if ((run.flags & CODE) && run.width > 0)
        renderer.drawLine(left + run.x, top + run.y + renderer.getLineHeight(run.font) - 1,
                          left + run.x + run.width - 1, top + run.y + renderer.getLineHeight(run.font) - 1, black);
    }
    renderer.drawText(run.font, left + run.x, top + run.y, work->pageText + run.offset, black, fontStyle(run.flags));
  }
}
