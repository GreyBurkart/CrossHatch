#pragma once

#include <cstdint>
#include <memory>
#include <string>

class GfxRenderer;
class Txt;

// Rebuildable SD spool: only one page and a bounded parsing workspace live in RAM.
// No document tree, second framebuffer, or document-sized page index is allocated.
class MarkdownReaderLayout {
 public:
  struct Config {
    int fontId;
    int headingFontId;
    int width;
    int height;
    int lineHeight;
  };

  MarkdownReaderLayout();
  ~MarkdownReaderLayout();
  bool build(const Txt& txt, GfxRenderer& renderer, const Config& config);
  bool loadPage(int page);
  void draw(GfxRenderer& renderer, int left, int top, bool black) const;
  int pageCount() const { return pages; }
  bool sourceOffset(int page, uint32_t& offset) const;

 private:
  struct Workspace;
  std::unique_ptr<Workspace> work;
  std::string dataPath;
  std::string indexPath;
  int pages = 0;
};
