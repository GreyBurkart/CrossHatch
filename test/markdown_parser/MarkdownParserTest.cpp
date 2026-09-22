#include <gtest/gtest.h>
#include <util/MarkdownParser.h>

#include <array>
#include <string>
#include <vector>

namespace {
using namespace MarkdownParser;
struct InlineResult {
  std::string text;
  std::vector<uint8_t> styles;
};
InlineResult parse(std::string_view source) {
  std::vector<char> text(source.size() + 1);
  std::vector<uint8_t> styles(source.size() + 1);
  size_t length = 123;
  EXPECT_TRUE(parseInline(source, text.data(), styles.data(), text.size(), length));
  EXPECT_LE(length, source.size());
  EXPECT_EQ(text[length], '\0');
  styles.resize(length);
  return {std::string(text.data(), length), styles};
}

TEST(MarkdownParser, ClassifiesHeadingsAndOptionalTrailingHashes) {
  FenceState state;
  for (uint8_t level = 1; level <= 6; ++level) {
    const auto source = std::string(level, '#') + " Heading ### \r";
    const Block block = classifyLine(source, state);
    EXPECT_EQ(block.kind, BlockKind::Heading);
    EXPECT_EQ(block.headingLevel, level);
    EXPECT_EQ(block.content, "Heading");
  }
  EXPECT_EQ(classifyLine("#", state).kind, BlockKind::Heading);
  EXPECT_EQ(classifyLine("# C#", state).content, "C#");
  EXPECT_EQ(classifyLine("####### Too many", state).kind, BlockKind::Paragraph);
  EXPECT_EQ(classifyLine("#hashtag", state).kind, BlockKind::Paragraph);
  EXPECT_EQ(classifyLine("    # indented", state).kind, BlockKind::Paragraph);
}

TEST(MarkdownParser, ClassifiesOrderedAndNestedUnorderedLists) {
  FenceState state;
  for (const std::string_view source : {"- Item", "+ Item", "* Item", "1. Item", "123) Item"}) {
    const Block block = classifyLine(source, state);
    EXPECT_EQ(block.kind, BlockKind::ListItem);
    EXPECT_EQ(block.content, "Item");
    EXPECT_EQ(block.marker, source.substr(0, source.find(' ')));
  }
  const Block nested = classifyLine("  \t- [ ] A task", state);
  EXPECT_EQ(nested.kind, BlockKind::ListItem);
  EXPECT_EQ(nested.indent, 6u);
  EXPECT_EQ(nested.content, "[ ] A task");
  EXPECT_EQ(classifyLine("1.23 decimal", state).kind, BlockKind::Paragraph);
  EXPECT_EQ(classifyLine("1234567890. Too many digits", state).kind, BlockKind::Paragraph);
}

TEST(MarkdownParser, RulesTakePriorityOverListMarkers) {
  FenceState state;
  for (const std::string_view source : {"---", "* * *", "  ___", "- - - -\t"}) {
    EXPECT_EQ(classifyLine(source, state).kind, BlockKind::Rule) << source;
  }
  EXPECT_EQ(classifyLine("- -", state).kind, BlockKind::ListItem);
  EXPECT_EQ(classifyLine("- * -", state).kind, BlockKind::ListItem);
  EXPECT_EQ(classifyLine("---label", state).kind, BlockKind::Paragraph);
}

TEST(MarkdownParser, ClassifiesQuotesBlanksAndPlainText) {
  FenceState state;
  EXPECT_EQ(classifyLine("", state).kind, BlockKind::Blank);
  EXPECT_EQ(classifyLine(" \t\r", state).kind, BlockKind::Blank);
  const Block quote = classifyLine(" > Quoted text \r", state);
  EXPECT_EQ(quote.kind, BlockKind::Quote);
  EXPECT_EQ(quote.content, "Quoted text");
  EXPECT_EQ(classifyLine(">", state).kind, BlockKind::Quote);
  EXPECT_EQ(classifyLine("Ordinary prose", state).kind, BlockKind::Paragraph);
}

TEST(MarkdownParser, FenceStateRequiresMatchingTypeLengthAndWhitespace) {
  FenceState state;
  EXPECT_EQ(classifyLine("````cpp", state).kind, BlockKind::Fence);
  EXPECT_EQ(state.marker, '`');
  EXPECT_EQ(state.length, 4u);
  for (const std::string_view source : {"  # literal", "```", "~~~~", "```` trailing", "    ````", ""}) {
    const Block block = classifyLine(source, state);
    EXPECT_EQ(block.kind, BlockKind::Code) << source;
    EXPECT_EQ(block.content, source);
    EXPECT_EQ(state.marker, '`');
  }
  EXPECT_EQ(classifyLine("  ````` \t", state).kind, BlockKind::Fence);
  EXPECT_EQ(state.marker, 0);
  EXPECT_EQ(classifyLine("# Heading", state).kind, BlockKind::Heading);
}

TEST(MarkdownParser, TildeFencesAndCopiedCheckpointsRetainContext) {
  FenceState state;
  EXPECT_EQ(classifyLine("~~~language", state).kind, BlockKind::Fence);
  FenceState checkpoint = state;
  EXPECT_EQ(classifyLine("**literal**", checkpoint).kind, BlockKind::Code);
  EXPECT_EQ(classifyLine("~~~", checkpoint).kind, BlockKind::Fence);
  EXPECT_EQ(state.marker, '~');
  FenceState plain;
  EXPECT_EQ(classifyLine("```bad`info", plain).kind, BlockKind::Paragraph);
  EXPECT_EQ(plain.marker, 0);
}

TEST(MarkdownParser, PlainNonTerminatedViewsRemainBounded) {
  const char source[] = {'a', 'b', 'c', '*', '*'};
  const auto result = parse(std::string_view(source, 3));
  EXPECT_EQ(result.text, "abc");
  EXPECT_EQ(result.styles, (std::vector<uint8_t>{Regular, Regular, Regular}));
  EXPECT_EQ(parse("").text, "");
}

TEST(MarkdownParser, AppliesStrongEmphasisAndCombinedStyles) {
  for (const auto& [source, style] :
       {std::pair{"**word**", Bold}, {"__word__", Bold}, {"*word*", Italic}, {"_word_", Italic}}) {
    const auto result = parse(source);
    EXPECT_EQ(result.text, "word");
    EXPECT_EQ(result.styles, std::vector<uint8_t>(4, style));
  }
  for (const std::string_view source : {"***word***", "___word___", "**_word_**", "*__word__*"}) {
    const auto result = parse(source);
    EXPECT_EQ(result.text, "word");
    EXPECT_EQ(result.styles, std::vector<uint8_t>(4, Bold | Italic));
  }
}

TEST(MarkdownParser, SupportsNestedSameMarkerEmphasis) {
  for (const std::string_view source : {"**bold *both***", "*italic **both***"}) {
    const auto result = parse(source);
    EXPECT_EQ(result.text, source.front() == '*' && source[1] == '*' ? "bold both" : "italic both");
    for (size_t at = result.text.size() - 4; at < result.text.size(); ++at) {
      EXPECT_EQ(result.styles[at], Bold | Italic);
    }
  }
}

TEST(MarkdownParser, KeepsMalformedAndIntrawordUnderscoresLiteral) {
  for (const std::string_view source : {"unfinished *emphasis", "**no end", "* padded*", "*padded *", "****wide****",
                                        "snake_case_name", "some__name__here", "pré_nom_suite", "_open", "close_"}) {
    const auto result = parse(source);
    EXPECT_EQ(result.text, source) << source;
    EXPECT_EQ(result.styles, std::vector<uint8_t>(source.size(), Regular)) << source;
  }
  EXPECT_EQ(parse("an _emphasized_ word").text, "an emphasized word");
}

TEST(MarkdownParser, InlineCodeProtectsMarkupAndSupportsBackticksInside) {
  for (const std::string_view source : {"`*raw*`", "``*raw*``", "` *raw* `"}) {
    const auto result = parse(source);
    EXPECT_EQ(result.text, "*raw*");
    EXPECT_EQ(result.styles, std::vector<uint8_t>(5, Code));
  }
  const auto result = parse("``a ` b``");
  EXPECT_EQ(result.text, "a ` b");
  EXPECT_EQ(result.styles, std::vector<uint8_t>(5, Code));
  EXPECT_EQ(parse("`   `").text, "   ");
  EXPECT_EQ(parse("``unmatched`").text, "``unmatched`");
  EXPECT_EQ(parse("`\\*literal\\*`").text, "\\*literal\\*");
}

TEST(MarkdownParser, EscapesOnlyAsciiPunctuation) {
  const auto result = parse("\\*literal\\* \\_plain\\_ \\[x\\] \\a \\\\ \\`code\\`");
  EXPECT_EQ(result.text, "*literal* _plain_ [x] \\a \\ `code`");
  EXPECT_EQ(result.styles, std::vector<uint8_t>(result.text.size(), Regular));
}

TEST(MarkdownParser, LinkLabelsCanBeStyledAndDestinationsCanHaveParentheses) {
  EXPECT_EQ(parse("Read [this note](https://example.org/a_(b)) now").text, "Read this note now");
  const auto result = parse("[**bold** label](destination \"a title\")");
  EXPECT_EQ(result.text, "bold label");
  EXPECT_EQ(result.styles[0], Bold);
  EXPECT_EQ(result.styles[4], Regular);
  EXPECT_EQ(parse("[label]()").text, "label");
  EXPECT_EQ(parse("[](destination)").text, "");
  EXPECT_EQ(parse("[bracket\\]](destination)").text, "bracket]");
}

TEST(MarkdownParser, UnsupportedLinksImagesAndSyntaxRemainReadable) {
  for (const std::string_view source : {"[unfinished](url", "[reference][id]", "[[Obsidian link]]", "![alt](image.png)",
                                        "<b>HTML</b>", "| A | B |", "[nested [label]](url)"}) {
    EXPECT_EQ(parse(source).text, source) << source;
  }
}

TEST(MarkdownParser, CopiesUtf8AndMalformedBytesWithoutSplittingStyles) {
  const std::string body = "café 日本語 \xFF\x80";
  const auto result = parse("**" + body + "**");
  EXPECT_EQ(result.text, body);
  EXPECT_EQ(result.styles, std::vector<uint8_t>(body.size(), Bold));
}

TEST(MarkdownParser, OutputCapacityFailureIsExplicitAndDoesNotTruncate) {
  std::array<char, 8> text;
  std::array<uint8_t, 8> styles;
  text.fill('!');
  styles.fill(99);
  size_t length = 123;
  EXPECT_FALSE(parseInline("12345678", text.data(), styles.data(), text.size(), length));
  EXPECT_EQ(length, 0u);
  EXPECT_EQ(text[0], '\0');
  EXPECT_EQ(text[1], '!');
  EXPECT_EQ(styles[0], 99);
  EXPECT_TRUE(parseInline("1234567", text.data(), styles.data(), text.size(), length));
  EXPECT_EQ(length, 7u);
  EXPECT_EQ(text[7], '\0');
  EXPECT_FALSE(parseInline("x", nullptr, styles.data(), styles.size(), length));
  EXPECT_FALSE(parseInline("x", text.data(), nullptr, text.size(), length));
  EXPECT_FALSE(parseInline("", text.data(), styles.data(), 0, length));
}

TEST(MarkdownParser, BoundedNestingFallsBackToLiteralWithoutLosingWords) {
  const auto result =
      parse("***[***[***[***[***[***[***[***[***word***](a)***](a)***](a)***](a)***](a)***](a)***](a)***](a)***");
  EXPECT_NE(result.text.find("word"), std::string::npos);
}

TEST(MarkdownParser, ArbitraryBytesRespectOutputBounds) {
  uint32_t seed = 0xAC03D123;
  constexpr std::string_view alphabet = "a *_[`](!)~\\\n\t\xFF\x80";
  for (size_t trial = 0; trial < 500; ++trial) {
    std::string source;
    source.reserve(128);
    for (size_t at = 0; at < trial % 128; ++at) {
      seed = seed * 1664525u + 1013904223u;
      source.push_back(alphabet[seed % alphabet.size()]);
    }
    std::vector<char> text(source.size() + 2, '!');
    std::vector<uint8_t> styles(source.size() + 2, 99);
    size_t length = 0;
    ASSERT_TRUE(parseInline(source, text.data(), styles.data(), source.size() + 1, length));
    ASSERT_LE(length, source.size());
    EXPECT_EQ(text[length], '\0');
    EXPECT_EQ(text.back(), '!');
    EXPECT_EQ(styles.back(), 99);
  }
}
}  // namespace
