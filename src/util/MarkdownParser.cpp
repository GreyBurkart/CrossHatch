#include "MarkdownParser.h"

#include <limits>

namespace MarkdownParser {
namespace {
bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool word(char c) {
  const auto byte = static_cast<unsigned char>(c);
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || byte >= 128;
}
bool punctuation(char c) {
  return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') || (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}
size_t runLength(std::string_view text, size_t at) {
  size_t end = at + 1;
  while (end < text.size() && text[end] == text[at]) ++end;
  return end - at;
}
std::string_view trimRight(std::string_view text) {
  while (!text.empty() && space(text.back())) text.remove_suffix(1);
  return text;
}
bool onlySpace(std::string_view text) {
  for (const char c : text) {
    if (!space(c)) return false;
  }
  return true;
}
size_t codeEnd(std::string_view text, size_t start, size_t count) {
  for (size_t at = start + count; at < text.size();) {
    if (text[at] != '`') {
      ++at;
      continue;
    }
    const size_t run = runLength(text, at);
    if (run == count) return at;
    at += run;
  }
  return std::string_view::npos;
}
size_t emphasisEnd(std::string_view text, size_t start, size_t count) {
  const char marker = text[start];
  for (size_t at = start + count; at < text.size();) {
    if (text[at] == '\\' && at + 1 < text.size() && punctuation(text[at + 1])) {
      at += 2;
      continue;
    }
    if (text[at] == '`') {
      const size_t ticks = runLength(text, at);
      const size_t end = codeEnd(text, at, ticks);
      at = end == std::string_view::npos ? at + ticks : end + ticks;
      continue;
    }
    if (text[at] != marker) {
      ++at;
      continue;
    }
    const size_t run = runLength(text, at);
    if (run >= count && !space(text[at - 1]) && !(marker == '_' && at + run < text.size() && word(text[at + run]))) {
      // In **bold *italic***, reserve the first trailing star for the inner
      // emphasis. The same rule handles *italic **bold***.
      if (run <= 3) return at + run - count;
    }
    at += run;
  }
  return std::string_view::npos;
}
bool linkEnd(std::string_view text, size_t start, size_t& labelEnd, size_t& end) {
  size_t at = start + 1;
  while (at < text.size()) {
    if (text[at] == '\\' && at + 1 < text.size() && punctuation(text[at + 1])) {
      at += 2;
      continue;
    }
    if (text[at] == '[' || text[at] == '\n') return false;
    if (text[at] == ']') break;
    ++at;
  }
  if (at + 1 >= text.size() || text[at + 1] != '(') return false;
  labelEnd = at;
  unsigned depth = 1;
  for (at += 2; at < text.size(); ++at) {
    if (text[at] == '\\' && at + 1 < text.size() && punctuation(text[at + 1])) {
      ++at;
      continue;
    }
    if (text[at] == '\n') return false;
    if (text[at] == '(' && ++depth > 8) return false;
    if (text[at] == ')' && --depth == 0) {
      end = at + 1;
      return true;
    }
  }
  return false;
}
}  // namespace

Block classifyLine(std::string_view line, FenceState& fence) {
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
  Block block;
  block.content = line;
  size_t first = 0;
  while (first < line.size() && (line[first] == ' ' || line[first] == '\t')) {
    block.indent += line[first] == '\t' ? 4 : 1;
    ++first;
  }
  const auto body = line.substr(first);
  if (fence.marker) {
    if (block.indent <= 3 && !body.empty() && body.front() == fence.marker) {
      const size_t count = runLength(body, 0);
      if (count >= fence.length && onlySpace(body.substr(count))) {
        fence = {};
        block.kind = BlockKind::Fence;
        block.content = {};
        return block;
      }
    }
    block.kind = BlockKind::Code;
    return block;
  }
  if (body.empty()) {
    block.kind = BlockKind::Blank;
    block.content = {};
    return block;
  }
  if (block.indent <= 3 && (body.front() == '`' || body.front() == '~')) {
    const size_t count = runLength(body, 0);
    if (count >= 3 && (body.front() != '`' || body.substr(count).find('`') == std::string_view::npos)) {
      fence.marker = body.front();
      fence.length = count;
      block.kind = BlockKind::Fence;
      block.content = {};
      return block;
    }
  }
  if (block.indent <= 3 && body.front() == '#') {
    const size_t count = runLength(body, 0);
    if (count <= 6 && (count == body.size() || space(body[count]))) {
      size_t start = count;
      while (start < body.size() && space(body[start])) ++start;
      block.kind = BlockKind::Heading;
      block.headingLevel = static_cast<uint8_t>(count);
      block.content = trimRight(body.substr(start));
      size_t tail = block.content.size();
      while (tail > 0 && block.content[tail - 1] == '#') --tail;
      if (tail > 0 && tail < block.content.size() && space(block.content[tail - 1])) {
        block.content = trimRight(block.content.substr(0, tail));
      }
      return block;
    }
  }
  if (block.indent <= 3 && (body.front() == '-' || body.front() == '*' || body.front() == '_')) {
    size_t markers = 0;
    bool rule = true;
    for (const char c : body) {
      if (c == body.front())
        ++markers;
      else if (!space(c))
        rule = false;
    }
    if (rule && markers >= 3) {
      block.kind = BlockKind::Rule;
      block.content = {};
      return block;
    }
  }
  size_t markerLength = 0;
  if ((body.front() == '-' || body.front() == '*' || body.front() == '+') && (body.size() == 1 || space(body[1]))) {
    markerLength = 1;
  } else {
    size_t digits = 0;
    while (digits < body.size() && body[digits] >= '0' && body[digits] <= '9') ++digits;
    if (digits >= 1 && digits <= 9 && digits < body.size() && (body[digits] == '.' || body[digits] == ')') &&
        (digits + 1 == body.size() || space(body[digits + 1]))) {
      markerLength = digits + 1;
    }
  }
  if (markerLength) {
    block.kind = BlockKind::ListItem;
    block.marker = body.substr(0, markerLength);
    size_t start = markerLength;
    while (start < body.size() && space(body[start])) ++start;
    block.content = trimRight(body.substr(start));
    return block;
  }
  if (block.indent <= 3 && body.front() == '>') {
    block.kind = BlockKind::Quote;
    size_t start = 1;
    if (start < body.size() && space(body[start])) ++start;
    block.content = trimRight(body.substr(start));
    return block;
  }
  block.content = trimRight(body);
  return block;
}

bool parseInline(std::string_view input, char* text, uint8_t* styles, size_t capacity, size_t& length) {
  length = 0;
  if (text && capacity) text[0] = '\0';
  if (!text || !styles || input.size() == std::numeric_limits<size_t>::max() || capacity <= input.size()) return false;

  // At most eight open constructs: 192 bytes on a 64-bit host, 96 on ESP32.
  // An explicit stack keeps nesting bounded without recursive task-stack use.
  struct Frame {
    size_t end;
    size_t resume;
    uint8_t previousStyle;
  };
  Frame frames[8];
  size_t depth = 0;
  size_t at = 0;
  uint8_t style = Regular;
  while (at < input.size()) {
    if (depth && at == frames[depth - 1].end) {
      const auto& frame = frames[--depth];
      at = frame.resume;
      style = frame.previousStyle;
      continue;
    }
    const size_t end = depth ? frames[depth - 1].end : input.size();
    const auto current = input.substr(0, end);
    const char c = input[at];
    if (c == '\\' && at + 1 < end && punctuation(input[at + 1])) {
      text[length] = input[at + 1];
      styles[length++] = style;
      at += 2;
      continue;
    }
    if (c == '`') {
      const size_t count = runLength(current, at);
      const size_t close = codeEnd(current, at, count);
      if (close != std::string_view::npos) {
        size_t begin = at + count;
        size_t codeLimit = close;
        if (codeLimit > begin + 1 && input[begin] == ' ' && input[codeLimit - 1] == ' ' &&
            !onlySpace(input.substr(begin, codeLimit - begin))) {
          ++begin;
          --codeLimit;
        }
        for (size_t index = begin; index < codeLimit; ++index) {
          text[length] = input[index];
          styles[length++] = static_cast<uint8_t>(style | Code);
        }
        at = close + count;
        continue;
      }
      // Keep the entire unmatched run; later ticks must not reinterpret it.
      for (size_t index = 0; index < count; ++index) {
        text[length] = c;
        styles[length++] = style;
      }
      at += count;
      continue;
    }
    if (depth < 8 && c == '[' && (at == 0 || input[at - 1] != '!')) {
      size_t labelEnd = 0;
      size_t resume = 0;
      if (linkEnd(current, at, labelEnd, resume)) {
        frames[depth++] = {labelEnd, resume, style};
        ++at;
        continue;
      }
    }
    if (c == '*' || c == '_') {
      const size_t count = runLength(current, at);
      if (depth < 8 && count <= 3 && at + count < end && !space(input[at + count]) &&
          !(c == '_' && at > 0 && word(input[at - 1]))) {
        const size_t close = emphasisEnd(current, at, count);
        if (close != std::string_view::npos && close > at + count) {
          frames[depth++] = {close, close + count, style};
          style |= count == 1 ? Italic : count == 2 ? Bold : static_cast<uint8_t>(Bold | Italic);
          at += count;
          continue;
        }
      }
      for (size_t index = 0; index < count; ++index) {
        text[length] = c;
        styles[length++] = style;
      }
      at += count;
      continue;
    }
    text[length] = c;
    styles[length++] = style;
    ++at;
  }
  text[length] = '\0';
  styles[length] = Regular;
  return true;
}
}  // namespace MarkdownParser
