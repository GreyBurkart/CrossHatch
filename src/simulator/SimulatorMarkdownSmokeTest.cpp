#ifdef SIMULATOR

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cstdlib>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "GlobalActions.h"
#include "activities/ActivityManager.h"
#include "activities/reader/ReaderUtils.h"
#include "activities/reader/TxtReaderActivity.h"
#include "components/UITheme.h"
#include "util/MarkdownChecklist.h"
#include "util/ScreenshotUtil.h"

extern ActivityManager activityManager;
extern GfxRenderer renderer;
extern MappedInputManager mappedInputManager;

namespace {
using Button = MappedInputManager::Button;
constexpr const char* MARKDOWN = "/books/markdown.md";
constexpr const char* CODE = "/books/code.md";
constexpr const char* CHECKLIST = "/books/tasks.md";
bool releasePending = false;
Button releaseButton = Button::PageForward;

void require(const bool value, const char* message) {
  if (value) return;
  LOG_ERR("MDTEST", "%s", message);
  std::_Exit(2);
}

void tap(const Button button) {
  mappedInputManager.simulatorInjectPress(button);
  releasePending = true;
  releaseButton = button;
}

void saveFramebuffer(const char* path) {
  require(ScreenshotUtil::saveFramebufferAsBmp(path, renderer.getFrameBuffer(), renderer.getDisplayWidth(),
                                               renderer.getDisplayHeight()),
          "Screenshot failed");
}

void capture(const char* path) {
  require(activityManager.requestUpdateAndWait() == RequestUpdateResult::Rendered, "Render failed");
  RenderLock lock;
  saveFramebuffer(path);
}

// Logical coordinates keep this comparison valid for portrait and landscape
// hardware profiles. Exclude the status bar because sleep snapshots omit it.
uint32_t bodyHash() {
  int top = 0, right = 0, bottom = 0, left = 0;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  top += SETTINGS.screenMarginVertical;
  bottom += std::max(static_cast<int>(SETTINGS.screenMarginVertical),
                     UITheme::getInstance().getStatusBarHeight() + ReaderUtils::STATUS_BAR_TEXT_PADDING);
  left += SETTINGS.screenMarginHorizontal;
  right += SETTINGS.screenMarginHorizontal;
  uint32_t hash = 2166136261u;
  for (int y = top; y < renderer.getScreenHeight() - bottom; ++y) {
    for (int x = left; x < renderer.getScreenWidth() - right; ++x) {
      hash = (hash ^ static_cast<uint32_t>(renderer.isPixelBlack(x, y))) * 16777619u;
    }
  }
  return hash;
}

uint32_t renderedBodyHash() {
  require(activityManager.requestUpdateAndWait() == RequestUpdateResult::Rendered, "Render failed");
  RenderLock lock;
  return bodyHash();
}

std::array<uint8_t, 6> position(const char* path) {
  require(activityManager.prepareForDocumentSwitch(), "Progress flush failed");
  const std::string cache = "/.crosspoint/txt_" + std::to_string(std::hash<std::string>{}(path));
  auto file = Storage.open((cache + "/progress.bin").c_str());
  require(static_cast<bool>(file), "Progress file missing");
  std::array<uint8_t, 6> bytes{};
  require(file.size() == bytes.size() && file.read(bytes.data(), bytes.size()) == static_cast<int>(bytes.size()),
          "Could not read saved text position");
  file.close();
  return bytes;
}

void expectReader(const char* path) {
  require(activityManager.isCurrentActivityNamed("TxtReader") && activityManager.getCurrentBookPath() == path,
          "Expected text reader did not open");
}

void verifySleepSnapshot(const char* path, const char* screenshot) {
  position(path);
  const uint32_t expected = renderedBodyHash();
  RenderLock lock;
  // A reconstruction must replace unrelated framebuffer contents, not merely
  // draw over an identical page left behind by the foreground reader.
  renderer.clearScreen(0x00);
  require(TxtReaderActivity::drawCurrentPageToBuffer(path, renderer), "Saved Markdown sleep page failed to load");
  require(bodyHash() == expected, "Sleep page differs from the formatted reading page");
  saveFramebuffer(screenshot);
}
}  // namespace

bool runSimulatorMarkdownSmokeTestTick() {
  if (!std::getenv("CROSSHATCH_MARKDOWN_SMOKE")) return false;
  static unsigned step = 0;
  static unsigned settle = 0;
  static uint8_t initialFontFamily = 0;
  static uint32_t firstHash = 0;
  static uint32_t secondHash = 0;
  static uint32_t fontHash = 0;
  static uint32_t codeHash = 0;
  static std::array<uint8_t, 6> firstPosition{};
  static std::array<uint8_t, 6> secondPosition{};
  static std::array<uint8_t, 6> fontPosition{};
  mappedInputManager.simulatorClearInputFrame();
  if (releasePending) {
    mappedInputManager.simulatorInjectRelease(releaseButton);
    releasePending = false;
    settle = 2;
    return true;
  }
  if (settle) {
    if (!--settle) require(activityManager.requestUpdateAndWait() == RequestUpdateResult::Rendered, "Settle failed");
    return true;
  }
  LOG_INF("MDTEST", "Step %u", step);
  switch (step++) {
    case 0:
      if (const auto* theme = std::getenv("CROSSINK_SIMULATOR_SMOKE_THEME")) {
        SETTINGS.uiTheme = std::atoi(theme);
        UITheme::getInstance().reload();
      }
      SETTINGS.hideClock = CrossPointSettings::HIDE_CLOCK_ALWAYS;
      SETTINGS.textAntiAliasing = false;
      initialFontFamily = SETTINGS.fontFamily;
      require(activityManager.goToReader(MARKDOWN), "Markdown open rejected");
      break;
    case 1:
      expectReader(MARKDOWN);
      require(activityManager.getScreenshotInfo().totalPages > 2, "Markdown fixture did not paginate");
      firstPosition = position(MARKDOWN);
      firstHash = renderedBodyHash();
      capture("/markdown-first.bmp");
      tap(Button::PageForward);
      break;
    case 2:
      secondPosition = position(MARKDOWN);
      secondHash = renderedBodyHash();
      require(secondPosition != firstPosition && secondHash != firstHash, "Markdown page did not advance");
      capture("/markdown-second.bmp");
      require(dispatchShortcutAction(CrossPointSettings::PREVIOUS_PAGE), "Previous page rejected");
      break;
    case 3:
      require(position(MARKDOWN) == firstPosition && renderedBodyHash() == firstHash,
              "Returning to first page changed text layout or progress");
      tap(Button::PageForward);
      break;
    case 4:
      require(position(MARKDOWN) == secondPosition && renderedBodyHash() == secondHash,
              "Returning to second page changed text layout or progress");
      verifySleepSnapshot(MARKDOWN, "/markdown-sleep.bmp");
      activityManager.goHome();
      break;
    case 5:
      require(activityManager.isHomeActivity(), "Home did not open");
      require(activityManager.goToReader(MARKDOWN), "Markdown reopen rejected");
      break;
    case 6:
      expectReader(MARKDOWN);
      require(position(MARKDOWN) == secondPosition && renderedBodyHash() == secondHash,
              "Markdown reopen lost saved position or formatting");
      require(dispatchShortcutAction(CrossPointSettings::TOGGLE_FONT), "Font change rejected");
      break;
    case 7:
      require(SETTINGS.fontFamily != initialFontFamily, "Font did not change");
      require(activityManager.getScreenshotInfo().currentPage > 0, "Font rebuild lost reading page");
      fontPosition = position(MARKDOWN);
      fontHash = renderedBodyHash();
      require(fontHash != secondHash, "Font change did not rebuild visible text");
      capture("/markdown-font.bmp");
      verifySleepSnapshot(MARKDOWN, "/markdown-font-sleep.bmp");
      activityManager.goHome();
      break;
    case 8:
      require(activityManager.goToReader(MARKDOWN), "Changed-font reopen rejected");
      break;
    case 9:
      expectReader(MARKDOWN);
      require(position(MARKDOWN) == fontPosition, "Changed-font reopen changed the saved page/source position");
      require(renderedBodyHash() == fontHash, "Changed-font reopen changed the rendered page");
      SETTINGS.fontFamily = initialFontFamily;
      require(activityManager.goToReader("/books/markdown.txt"), "Plain text open rejected");
      break;
    case 10:
      expectReader("/books/markdown.txt");
      require(renderedBodyHash() != firstHash, "Plain TXT unexpectedly rendered Markdown formatting");
      capture("/markdown-literal-txt.bmp");
      require(activityManager.goToReader("/books/uppercase.MD"), "Uppercase Markdown open rejected");
      break;
    case 11:
      expectReader("/books/uppercase.MD");
      require(renderedBodyHash() == firstHash, "Uppercase Markdown did not use the same formatting");
      require(activityManager.goToReader(CHECKLIST), "Checklist open rejected");
      break;
    case 12:
      require(activityManager.isCurrentActivityNamed("Checklist"), "Markdown tasks lost interactive checklist access");
      tap(Button::Up);  // Wrap to View Markdown text.
      break;
    case 13:
      tap(Button::Confirm);
      break;
    case 14: {
      expectReader(CHECKLIST);
      capture("/markdown-checklist-text.bmp");
      MarkdownChecklist document(CHECKLIST);
      require(document.load() == MarkdownChecklist::Status::Ok && document.completedCount() == 1,
              "Reading Markdown changed checklist completion");
      require(activityManager.goToReader(CODE), "Fenced code open rejected");
      break;
    }
    case 15:
      expectReader(CODE);
      require(activityManager.getScreenshotInfo().totalPages > 3, "Code fixture did not span pages");
      capture("/markdown-code-first.bmp");
      tap(Button::PageForward);
      break;
    case 16:
      codeHash = renderedBodyHash();
      capture("/markdown-code-second.bmp");
      verifySleepSnapshot(CODE, "/markdown-code-sleep.bmp");
      tap(Button::PageForward);
      break;
    case 17:
      capture("/markdown-code-third.bmp");
      require(dispatchShortcutAction(CrossPointSettings::PREVIOUS_PAGE), "Previous code page rejected");
      break;
    case 18:
      require(renderedBodyHash() == codeHash, "Returning across a code page boundary changed its formatting");
      activityManager.goHome();
      break;
    case 19:
      require(activityManager.goToReader(CODE), "Code reopen rejected");
      break;
    case 20:
      expectReader(CODE);
      require(renderedBodyHash() == codeHash, "Reopening inside a code fence lost formatting");
      capture("/markdown-code-reopened.bmp");
      LOG_INF("MDTEST", "Markdown reader smoke test passed");
      std::_Exit(0);
  }
  settle = 2;
  return true;
}
#endif
