#pragma once
#include <string>
#include <utility>

#include "activities/Activity.h"

#if defined(CROSSINK_ENABLE_PDF) && CROSSINK_ENABLE_PDF
#include "PdfSleepPageCache.h"
#include "RecentBooksStore.h"
#endif

class Bitmap;

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool canSnapshotOverlayBackground,
                         std::string currentBookPath = {}, bool fromTimeout = false,
                         GfxRenderer::Orientation sleepPopupOrientation = GfxRenderer::Orientation::Portrait)
      : Activity("Sleep", renderer, mappedInput),
        canSnapshotOverlayBackground(canSnapshotOverlayBackground),
        currentBookPath(std::move(currentBookPath)),
        fromTimeout(fromTimeout),
        sleepPopupOrientation(sleepPopupOrientation) {}
  void onEnter() override;

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  void renderReadingStatsSleepScreen() const;
  void renderMinimalSleepScreen() const;
  void renderMinimalStatsSleepScreen() const;
  void renderDashboardSleepScreen() const;
  bool renderBitmapSleepScreen(Bitmap& bitmap) const;
  void renderLastScreenSleepScreen() const;
  void renderBlankSleepScreen() const;
  void renderOverlaySleepScreen() const;
#if defined(CROSSINK_ENABLE_PDF) && CROSSINK_ENABLE_PDF
  void loadPdfSleepProducts(const std::string& path);
#endif
  bool canSnapshotOverlayBackground = false;
  bool overlayBackgroundBufferStored = false;
  std::string currentBookPath;
  bool fromTimeout = false;
  GfxRenderer::Orientation sleepPopupOrientation = GfxRenderer::Orientation::Portrait;
#if defined(CROSSINK_ENABLE_PDF) && CROSSINK_ENABLE_PDF
  PdfSleepProductCache pdfSleepProductCache;
  RecentBook pdfCachedBook;
  std::string pdfCachedChapter;
  float pdfCachedProgress = 0.0f;
  bool pdfBookHydrated = false;
#endif
};
