#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Txt.h>
#include <gtest/gtest.h>
#include <util/MarkdownReaderLayout.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {
constexpr const char* SOURCE = "/note.md";
constexpr const char* DATA = "/cache/markdown.pages";
constexpr const char* INDEX = "/cache/markdown.index";
std::string withoutSpace(std::string text) {
  text.erase(
      std::remove_if(text.begin(), text.end(), [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }),
      text.end());
  return text;
}

class MarkdownReaderLayoutTest : public ::testing::Test {
 protected:
  MarkdownReaderLayout layout;
  GfxRenderer renderer;
  MarkdownReaderLayout::Config config{1, 2, 100, 40, 10};
  Txt txt{SOURCE};

  void SetUp() override {
    fixture::reset();
    fixture::errors.clear();
  }
  void TearDown() override {
    EXPECT_EQ(fixture::overlapping, 0u);
    for (const auto& [path, handles] : fixture::handles) EXPECT_EQ(handles, 0u) << path;
  }
  bool build(const std::string& source) {
    fixture::files[SOURCE] = source;
    return layout.build(txt, renderer, config);
  }
  std::string renderAll() {
    std::string result;
    for (int page = 0; page < layout.pageCount(); ++page) {
      EXPECT_TRUE(layout.loadPage(page));
      renderer.clear();
      layout.draw(renderer, 0, 0, true);
      for (const auto& run : renderer.text) result += run.text;
    }
    return result;
  }
};

TEST_F(MarkdownReaderLayoutTest, ForwardBackwardLoadsAreDeterministic) {
  std::string source = "# My note\n\n";
  for (int row = 0; row < 60; ++row) source += "Paragraph " + std::to_string(row) + " has **bold** and *italic*.\n\n";
  ASSERT_TRUE(build(source));
  ASSERT_GT(layout.pageCount(), 20);
  std::vector<std::vector<GfxRenderer::Text>> pages;
  std::vector<std::vector<GfxRenderer::Line>> lines;
  uint32_t previousOffset = 0;
  for (int page = 0; page < layout.pageCount(); ++page) {
    ASSERT_TRUE(layout.loadPage(page));
    renderer.clear();
    layout.draw(renderer, 4, 8, true);
    pages.push_back(renderer.text);
    lines.push_back(renderer.lines);
    uint32_t offset = 0;
    ASSERT_TRUE(layout.sourceOffset(page, offset));
    EXPECT_GE(offset, previousOffset);
    EXPECT_LT(offset, source.size());
    previousOffset = offset;
  }
  for (int page = layout.pageCount() - 1; page >= 0; --page) {
    ASSERT_TRUE(layout.loadPage(page));
    renderer.clear();
    layout.draw(renderer, 4, 8, true);
    EXPECT_EQ(renderer.text, pages[page]);
    EXPECT_EQ(renderer.lines, lines[page]);
  }
  EXPECT_FALSE(layout.loadPage(-1));
  EXPECT_FALSE(layout.loadPage(layout.pageCount()));
}

TEST_F(MarkdownReaderLayoutTest, FencedCodeKeepsMarkupAndWhitespaceAcrossPages) {
  std::string source = "~~~~cpp\n";
  std::string expected;
  for (int row = 0; row < 30; ++row) {
    const std::string line = "  **raw_" + std::to_string(row) + "**();";
    source += line + "\n";
    expected += line;
  }
  source += "~~~~\n**after**";
  expected += "after";
  ASSERT_TRUE(build(source));
  ASSERT_GT(layout.pageCount(), 5);
  EXPECT_EQ(renderAll(), expected);
  EXPECT_FALSE(renderer.text.empty());
  EXPECT_EQ(renderer.text.back().style, EpdFontFamily::BOLD);
}

TEST_F(MarkdownReaderLayoutTest, QuotesAndListsRetainGuttersAcrossPages) {
  std::string source;
  std::string expected;
  for (int row = 0; row < 40; ++row) {
    const std::string line = "Quoted words " + std::to_string(row) + " remain readable.";
    source += "> " + line + "\n";
    expected += line;
  }
  source += "\n1. First list item with a long wrapped sentence.\n   Continued softly.\n2. Second item.\n";
  expected += "1. First list item with a long wrapped sentence. Continued softly.2. Second item.";
  ASSERT_TRUE(build(source));
  ASSERT_GT(layout.pageCount(), 10);
  EXPECT_EQ(withoutSpace(renderAll()), withoutSpace(expected));
  ASSERT_TRUE(layout.loadPage(1));
  renderer.clear();
  layout.draw(renderer, 0, 0, true);
  ASSERT_FALSE(renderer.text.empty());
  EXPECT_GE(renderer.text.front().x, config.lineHeight);
  EXPECT_TRUE(
      std::any_of(renderer.lines.begin(), renderer.lines.end(), [](const auto& line) { return line.x1 == line.x2; }));
}

TEST_F(MarkdownReaderLayoutTest, LongUtf8PhysicalLineKeepsEveryCharacterAtChunkBoundaries) {
  std::string source(2047, 'a');
  source += "日é🙂";
  for (int count = 0; count < 800; ++count) source += "é日本🙂";
  source += "tail";
  ASSERT_TRUE(build(source));
  ASSERT_GT(layout.pageCount(), 30);
  EXPECT_EQ(renderAll(), source);
  for (int page = 0; page < layout.pageCount(); ++page) {
    ASSERT_TRUE(layout.loadPage(page));
    renderer.clear();
    layout.draw(renderer, 0, 0, true);
    for (const auto& run : renderer.text) {
      ASSERT_FALSE(run.text.empty());
      EXPECT_NE(static_cast<unsigned char>(run.text.front()) & 0xc0, 0x80);
    }
  }
}

TEST_F(MarkdownReaderLayoutTest, ManySoftLinesAndLongParagraphChunksKeepAllWords) {
  std::string source;
  std::string expected;
  for (int word = 0; word < 1700; ++word) {
    const std::string token = "word" + std::to_string(word);
    source += token + (word % 7 == 0 ? "\n" : " ");
    expected += token;
  }
  ASSERT_TRUE(build(source));
  EXPECT_EQ(withoutSpace(renderAll()), expected);
}

TEST_F(MarkdownReaderLayoutTest, SameSizeSourceEditsReplacePriorSpool) {
  ASSERT_TRUE(build("# First\n\nOld words."));
  const auto first = renderAll();
  ASSERT_TRUE(build("# Later\n\nNew words."));
  const auto second = renderAll();
  EXPECT_NE(first, second);
  EXPECT_EQ(withoutSpace(second), "LaterNewwords.");
  ASSERT_TRUE(build("tiny"));
  EXPECT_EQ(layout.pageCount(), 1);
  EXPECT_EQ(renderAll(), "tiny");
}

TEST_F(MarkdownReaderLayoutTest, TinyViewportAlwaysMakesProgressWithoutDroppingGlyphs) {
  config.width = 1;
  config.height = 1;
  ASSERT_TRUE(build("é日本🙂 abc **bold**"));
  EXPECT_GT(layout.pageCount(), 5);
  EXPECT_EQ(withoutSpace(renderAll()), "é日本🙂abcbold");
}

TEST_F(MarkdownReaderLayoutTest, RunCapacityFlushesPreserveAllText) {
  config.width = 30000;
  config.height = 30000;
  std::string source;
  std::string expected;
  for (int word = 0; word < 2000; ++word) {
    source += "a **b** *c*\n";
    expected += "abc";
  }
  ASSERT_TRUE(build(source));
  ASSERT_GT(layout.pageCount(), 10);
  EXPECT_EQ(withoutSpace(renderAll()), expected);
}

TEST_F(MarkdownReaderLayoutTest, PageTextCapacityFlushesPreserveLongUnstyledText) {
  config.width = 30000;
  config.height = 30000;
  const std::string source(12000, 'a');
  ASSERT_TRUE(build(source));
  ASSERT_GE(layout.pageCount(), 3);
  EXPECT_EQ(renderAll(), source);
}

TEST_F(MarkdownReaderLayoutTest, HeadingInlineStylesAndRulesReachDrawing) {
  config.height = 200;
  ASSERT_TRUE(build("# Title\n\n## Second\n\n### Third\n\nplain **bold** *italic* ***both*** `code`\n\n---"));
  ASSERT_EQ(layout.pageCount(), 1);
  ASSERT_TRUE(layout.loadPage(0));
  layout.draw(renderer, 3, 7, true);
  ASSERT_FALSE(renderer.text.empty());
  EXPECT_EQ(renderer.text[0].font, config.headingFontId);
  EXPECT_EQ(renderer.text[0].style, EpdFontFamily::BOLD);
  for (const auto& run : renderer.text) {
    if (run.text == "Third") EXPECT_EQ(run.font, config.fontId);
    if (run.text == "both") EXPECT_EQ(run.style, EpdFontFamily::BOLD_ITALIC);
  }
  EXPECT_TRUE(std::any_of(renderer.lines.begin(), renderer.lines.end(), [&](const auto& line) {
    return line.x1 == 3 && line.x2 == 3 + config.width - 1 && line.y1 == line.y2;
  }));
  renderer.clear();
  renderer.mode = GfxRenderer::GRAYSCALE;
  layout.draw(renderer, 3, 7, true);
  EXPECT_TRUE(renderer.lines.empty());
  EXPECT_FALSE(renderer.text.empty());
}

TEST_F(MarkdownReaderLayoutTest, StyledRunPositionsUseActualKerningAdvance) {
  renderer.kerning = true;
  renderer.ligatures = true;
  for (const auto* source : {"**AV**tail", "**office**tail"}) {
    ASSERT_TRUE(build(source));
    ASSERT_TRUE(layout.loadPage(0));
    renderer.clear();
    layout.draw(renderer, 0, 0, true);
    ASSERT_EQ(renderer.text.size(), 2u);
    const auto& first = renderer.text[0];
    const auto& second = renderer.text[1];
    EXPECT_EQ(first.y, second.y);
    EXPECT_EQ(second.x, first.x + renderer.getTextAdvanceX(first.font, first.text.c_str(), first.style));
  }
}

TEST_F(MarkdownReaderLayoutTest, BlankCodeLinesOccupySpaceAtPageTopAndMiddle) {
  config.height = 20;
  ASSERT_TRUE(build("```\n\none\n\nthree\n```"));
  ASSERT_EQ(layout.pageCount(), 2);
  for (int page = 0; page < 2; ++page) {
    ASSERT_TRUE(layout.loadPage(page));
    renderer.clear();
    layout.draw(renderer, 0, 0, true);
    const auto visible =
        std::find_if(renderer.text.begin(), renderer.text.end(), [](const auto& run) { return !run.text.empty(); });
    ASSERT_NE(visible, renderer.text.end());
    EXPECT_EQ(visible->y, 10);
    EXPECT_EQ(visible->text, page == 0 ? "one" : "three");
  }
  config.height = 40;
  ASSERT_TRUE(build("```\none\n\nthree\n```"));
  ASSERT_TRUE(layout.loadPage(0));
  renderer.clear();
  layout.draw(renderer, 0, 0, true);
  ASSERT_EQ(renderer.text.size(), 3u);
  EXPECT_EQ(renderer.text[0].y, 0);
  EXPECT_EQ(renderer.text[2].y, 20);
}

TEST_F(MarkdownReaderLayoutTest, CodeIndentationIsNotAppliedTwice) {
  ASSERT_TRUE(build("```\n    x\n```"));
  ASSERT_TRUE(layout.loadPage(0));
  layout.draw(renderer, 0, 0, true);
  ASSERT_EQ(renderer.text.size(), 1u);
  EXPECT_EQ(renderer.text[0].text, "    x");
  EXPECT_EQ(renderer.text[0].x, config.lineHeight);
}

TEST_F(MarkdownReaderLayoutTest, ProseUsesConfiguredCompressedLineHeight) {
  renderer.bodyLineHeight = 12;
  ASSERT_TRUE(build("one  \ntwo  \nthree"));
  ASSERT_TRUE(layout.loadPage(0));
  layout.draw(renderer, 0, 0, true);
  ASSERT_EQ(renderer.text.size(), 3u);
  EXPECT_EQ(renderer.text[0].y, 0);
  EXPECT_EQ(renderer.text[1].y, 10);
  EXPECT_EQ(renderer.text[2].y, 20);
}

TEST_F(MarkdownReaderLayoutTest, TrailingSpacesAndBackslashPreserveExplicitLineBreaks) {
  ASSERT_TRUE(build("first  \nsecond\\\nthird"));
  ASSERT_TRUE(layout.loadPage(0));
  layout.draw(renderer, 0, 0, true);
  ASSERT_EQ(renderer.text.size(), 3u);
  EXPECT_EQ(renderer.text[0].text, "first");
  EXPECT_EQ(renderer.text[1].text, "second");
  EXPECT_EQ(renderer.text[2].text, "third");
  EXPECT_EQ(renderer.text[1].y - renderer.text[0].y, config.lineHeight);
  EXPECT_EQ(renderer.text[2].y - renderer.text[1].y, config.lineHeight);
}

TEST_F(MarkdownReaderLayoutTest, FailedSourceOffsetReadLeavesCallerPositionUntouched) {
  ASSERT_TRUE(build("Some readable text."));
  fixture::failRead = true;
  uint32_t offset = 1234;
  EXPECT_FALSE(layout.sourceOffset(0, offset));
  EXPECT_EQ(offset, 1234u);
  EXPECT_FALSE(fixture::errors.empty());
}

TEST_F(MarkdownReaderLayoutTest, ReadWriteAndSyncFailuresReturnFailureAndLog) {
  fixture::files[SOURCE] = "Text to render.";
  for (int failure = 0; failure < 3; ++failure) {
    fixture::failRead = failure == 0;
    fixture::failWrite = failure == 1;
    fixture::failSync = failure == 2;
    fixture::errors.clear();
    EXPECT_FALSE(layout.build(txt, renderer, config));
    EXPECT_EQ(layout.pageCount(), 0);
    EXPECT_FALSE(fixture::errors.empty());
    EXPECT_FALSE(fixture::files.count(DATA));
    EXPECT_FALSE(fixture::files.count(INDEX));
    renderer.clear();
    layout.draw(renderer, 0, 0, true);
    EXPECT_TRUE(renderer.text.empty());
  }
}

TEST_F(MarkdownReaderLayoutTest, OpenFailureClosesAlreadyOpenedFiles) {
  fixture::files[SOURCE] = "Source";
  fixture::failOpen.insert(INDEX);
  EXPECT_FALSE(layout.build(txt, renderer, config));
  EXPECT_EQ(layout.pageCount(), 0);
  EXPECT_FALSE(fixture::errors.empty());
}

TEST_F(MarkdownReaderLayoutTest, FailedPageReadClearsVisiblePreviousPage) {
  ASSERT_TRUE(build("A first page of ordinary prose."));
  ASSERT_TRUE(layout.loadPage(0));
  layout.draw(renderer, 0, 0, true);
  ASSERT_FALSE(renderer.text.empty());
  fixture::failRead = true;
  EXPECT_FALSE(layout.loadPage(0));
  EXPECT_FALSE(fixture::errors.empty());
  renderer.clear();
  layout.draw(renderer, 0, 0, true);
  EXPECT_TRUE(renderer.text.empty());
}

TEST_F(MarkdownReaderLayoutTest, TruncatedSpoolIsRejected) {
  ASSERT_TRUE(build("A page with **styles** and text."));
  auto& data = fixture::files[DATA];
  data.resize(data.size() - 2);
  EXPECT_FALSE(layout.loadPage(0));
  EXPECT_FALSE(fixture::errors.empty());
  layout.draw(renderer, 0, 0, true);
  EXPECT_TRUE(renderer.text.empty());
}
}  // namespace
