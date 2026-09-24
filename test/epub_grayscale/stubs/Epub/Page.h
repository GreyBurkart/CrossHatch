#pragma once
#include <Epub/blocks/ImageBlock.h>
#include <GfxRenderer.h>

#include <vector>
struct PdfPixelCacheRenderWorkspace;

class Page {
 public:
  struct PlacedImage {
    ImageBlock* image;
    int x, y;
  };
  std::vector<PlacedImage> images;
  mutable int allVisits = 0, imageVisits = 0;
  void renderImages(GfxRenderer& renderer, int, int x, int y) const {
    ++imageVisits;
    for (auto& item : images) item.image->render(renderer, x + item.x, y + item.y, true);
  }
  void render(GfxRenderer& renderer, int font, int x, int y, bool) const {
    ++allVisits;
    renderImages(renderer, font, x, y);
  }
  // The production Page threads an optional PDF pixel-cache workspace through.
  void renderImages(GfxRenderer& renderer, int font, int x, int y, PdfPixelCacheRenderWorkspace*) const {
    renderImages(renderer, font, x, y);
  }
  void render(GfxRenderer& renderer, int font, int x, int y, bool foregroundBlack,
              PdfPixelCacheRenderWorkspace*) const {
    render(renderer, font, x, y, foregroundBlack);
  }
};
