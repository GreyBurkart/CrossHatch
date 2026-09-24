#pragma once
#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "Block.h"

// One per PDF reader session: a reusable row buffer and path scratch for drawing
// PDF pixel caches without per-image heap allocation.
struct PdfPixelCacheRenderWorkspace {
  static constexpr size_t READ_BUFFER_BYTES = 4096;
  static constexpr size_t PATH_BYTES = 160;

  alignas(uint32_t) uint8_t readBuffer[READ_BUFFER_BYTES] = {};
  char path[PATH_BYTES] = {};
  bool inUse = false;
};

static_assert(sizeof(PdfPixelCacheRenderWorkspace) == 4260,
              "PDF pixel-cache workspace must retain its bounded aligned layout");

class ImageBlock final : public Block {
 public:
  ImageBlock(std::string imagePath, std::string sourcePath, int16_t width, int16_t height);
  ~ImageBlock() override = default;

  const std::string& getImagePath() const { return imagePath; }
  int16_t getWidth() const { return width; }
  int16_t getHeight() const { return height; }

  void prepareCache() const;
  bool hasValidCache() const;
  bool needsDecode() const;
  void renderPlaceholder(GfxRenderer& renderer, int x, int y, bool foregroundBlack) const;
  static void clearSessionRenderFailures();
  static void releaseSessionPixelCache();

  // The section builder only reads image headers. The reader supplies this
  // allocation-free callback pair to seed a bundled pixel cache or extract the
  // original image on its first render.
  using ExtractFn = bool (*)(void* context, const char* sourcePath, const char* destinationPath);
  using SeedCacheFn = bool (*)(void* context, const char* sourcePath, int width, int height,
                               const char* destinationPath);
  static void setExtractor(void* context, ExtractFn extract, SeedCacheFn seedCache);

  BlockType getType() override { return IMAGE_BLOCK; }
  bool isEmpty() override { return false; }

  void render(GfxRenderer& renderer, int x, int y, bool foregroundBlack);
  void render(GfxRenderer& renderer, int x, int y, bool foregroundBlack, PdfPixelCacheRenderWorkspace* pdfWorkspace);
  bool serialize(FsFile& file);
  static std::unique_ptr<ImageBlock> deserialize(FsFile& file);

 private:
  std::string imagePath;
  std::string sourcePath;
  int16_t width;
  int16_t height;

  static void* extractContext;
  static ExtractFn extractFn;
  static SeedCacheFn seedCacheFn;
};
