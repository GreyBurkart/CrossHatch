#pragma once

#include <string>
#include <tuple>
#include <vector>

namespace EpdFontFamily {
enum Style { REGULAR = 0, BOLD = 1, ITALIC = 2, BOLD_ITALIC = 3 };
}

class GfxRenderer {
 public:
  enum RenderMode { BW, GRAYSCALE };
  struct Text {
    int font;
    int x;
    int y;
    std::string text;
    bool black;
    EpdFontFamily::Style style;
    bool operator==(const Text&) const = default;
  };
  struct Line {
    int x1;
    int y1;
    int x2;
    int y2;
    bool black;
    bool operator==(const Line&) const = default;
  };
  std::vector<Text> text;
  std::vector<Line> lines;
  RenderMode mode = BW;
  bool kerning = false;
  bool ligatures = false;
  int bodyLineHeight = 10;
  bool isSdCardFont(int) const { return false; }
  void ensureSdCardFontReady(int, const char*, unsigned) {}
  int getLineHeight(int font) const { return font == 2 ? 14 : bodyLineHeight; }
  int getTextAdvanceX(int font, const char* value, EpdFontFamily::Style style) const {
    int width = 0;
    if (ligatures) {
      const std::string source(value);
      for (size_t at = source.find("ffi"); at != std::string::npos; at = source.find("ffi", at + 3)) width -= 3;
    }
    char previous = 0;
    for (; *value; ++value) {
      const unsigned char c = static_cast<unsigned char>(*value);
      if ((c & 0xc0) == 0x80) continue;
      width += c == ' ' ? 3 : c == 'i' || c == 'l' ? 2 : c == 'W' || c == 'M' ? 7 : 5;
      if (font == 2) ++width;
      if (style & EpdFontFamily::BOLD) ++width;
      if (kerning && previous == 'A' && c == 'V') width -= 2;
      previous = c;
    }
    return width;
  }
  RenderMode getRenderMode() const { return mode; }
  void drawText(int font, int x, int y, const char* value, bool black, EpdFontFamily::Style style) {
    text.push_back({font, x, y, value, black, style});
  }
  void drawLine(int x1, int y1, int x2, int y2, bool black) { lines.push_back({x1, y1, x2, y2, black}); }
  void clear() {
    text.clear();
    lines.clear();
  }
};
