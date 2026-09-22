#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// Deliberately small Markdown subset. Views refer to the caller's source;
// parsing uses no heap and never modifies the source document.
namespace MarkdownParser {
enum class BlockKind { Paragraph, Heading, ListItem, Quote, Code, Fence, Rule, Blank };
enum Style : uint8_t { Regular = 0, Bold = 1, Italic = 2, Code = 4 };

struct FenceState {
  char marker = 0;
  size_t length = 0;
};

struct Block {
  BlockKind kind = BlockKind::Paragraph;
  std::string_view content;
  std::string_view marker;
  uint8_t headingLevel = 0;
  size_t indent = 0;  // Leading columns, with tabs counted as four spaces.
};

// Classifies one physical line, excluding its newline. Fence state must be
// preserved at page checkpoints. Code content retains leading whitespace.
Block classifyLine(std::string_view line, FenceState& fence);

// Writes plain text plus a style for each output byte (including every byte of
// a UTF-8 character). text/styles must each have capacity >= input.size()+1;
// this guarantees that even entirely literal input fits without truncation.
// Output is NUL terminated; length excludes the terminator. Returns false with
// length=0 on invalid buffers or insufficient capacity. Input/output must not
// overlap. Unsupported or unmatched syntax is retained literally.
bool parseInline(std::string_view input, char* text, uint8_t* styles, size_t capacity, size_t& length);
}  // namespace MarkdownParser
