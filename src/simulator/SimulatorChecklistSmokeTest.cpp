#ifdef SIMULATOR

#include <HalStorage.h>
#include <Logging.h>

#include <cstdlib>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/TouchRegistry.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "util/MarkdownChecklist.h"
#include "util/ScreenshotUtil.h"

extern ActivityManager activityManager;
extern GfxRenderer renderer;
extern MappedInputManager mappedInputManager;

namespace {
using Button = MappedInputManager::Button;
constexpr const char* PATH = "/checklists/01-preshow.md";
bool releasePending = false;
Button releaseButton = Button::Confirm;
#if CROSSINK_APP_CAP_TOUCH
bool touchReleasePending = false;
int pendingTouchX = 0;
int pendingTouchY = 0;
#endif

void require(bool value, const char* message) {
  if (value) return;
  LOG_ERR("CHECKTEST", "%s", message);
  std::_Exit(2);
}
void tap(Button button) {
  mappedInputManager.simulatorInjectPress(button);
  releasePending = true;
  releaseButton = button;
}
void capture(const char* path) {
  require(activityManager.requestUpdateAndWait() == RequestUpdateResult::Rendered, "Render failed");
  RenderLock lock;
  require(ScreenshotUtil::saveFramebufferAsBmp(path, renderer.getFrameBuffer(), renderer.getDisplayWidth(),
                                               renderer.getDisplayHeight()),
          "Screenshot failed");
}
void expectCompleted(size_t count) {
  MarkdownChecklist document(PATH);
  require(document.load() == MarkdownChecklist::Status::Ok, "Saved Markdown could not reopen");
  require(document.completedCount() == count, "Wrong saved checkbox count");
}
}  // namespace

bool runSimulatorChecklistSmokeTestTick() {
  if (!std::getenv("CROSSHATCH_CHECKLIST_SMOKE")) return false;
  static unsigned step = 0;
  static unsigned settle = 0;
  static unsigned scroll = 0;
  mappedInputManager.simulatorClearInputFrame();
#if CROSSINK_APP_CAP_TOUCH
  if (touchReleasePending) {
    mappedInputManager.simulatorInjectTouchRelease(pendingTouchX, pendingTouchY);
    touchReleasePending = false;
    settle = 2;
    return true;
  }
#endif
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
  LOG_INF("CHECKTEST", "Step %u", step);
  switch (step++) {
    case 0:
      if (const auto* theme = std::getenv("CROSSINK_SIMULATOR_SMOKE_THEME")) {
        SETTINGS.uiTheme = std::atoi(theme);
        UITheme::getInstance().reload();
      }
      APP_STATE.openEpubPath = "/books/reading.epub";
      activityManager.goToFileBrowser("/checklists");
      break;
    case 1:
      require(activityManager.isCurrentActivityNamed("FileBrowser"), "Library did not open");
      capture("/phase3-library.bmp");
      tap(Button::Confirm);
      break;
    case 2:
      require(activityManager.isCurrentActivityNamed("Checklist"), "Markdown did not open as checklist");
      require(APP_STATE.openEpubPath == "/books/reading.epub", "Checklist replaced reading resume path");
      expectCompleted(1);
      capture("/phase3-checklist.bmp");
      tap(Button::Confirm);
      break;
    case 3:
      expectCompleted(2);
      tap(Button::Back);
      break;
    case 4:
      require(activityManager.isCurrentActivityNamed("FileBrowser"), "Back did not return to Library");
      activityManager.goToReader(PATH);
      break;
    case 5:
      require(activityManager.isCurrentActivityNamed("Checklist"), "Checklist did not reopen");
      expectCompleted(2);
      capture("/phase3-saved.bmp");
      tap(Button::Up);  // wrap to View Markdown text
      break;
    case 6:
      tap(Button::Up);  // Reset checklist
      break;
    case 7:
      capture("/phase3-actions.bmp");
      tap(Button::Confirm);
      break;
    case 8:
      require(activityManager.isCurrentActivityNamed("Confirmation"), "Reset did not ask for confirmation");
      capture("/phase3-reset-confirm.bmp");
      tap(Button::Back);
      break;
    case 9:
      require(activityManager.isCurrentActivityNamed("Checklist"), "Reset cancellation left checklist");
      expectCompleted(2);
      tap(Button::Confirm);
      break;
    case 10:
      tap(Button::Down);  // Choose Confirm instead of Cancel
      break;
    case 11:
      tap(Button::Confirm);
      break;
    case 12:
      require(activityManager.isCurrentActivityNamed("Checklist"), "Reset confirm did not return cleanly");
      expectCompleted(0);
      activityManager.goToReader(PATH);
      break;
    case 13: {
      expectCompleted(0);
      capture("/phase3-reset.bmp");
#if CROSSINK_APP_CAP_TOUCH
      if (mappedInputManager.hasTouchHardware()) {
        const auto& metrics = UITheme::getInstance().getMetrics();
        auto target = makeUiTarget(renderer);
        const auto tokens = uiThemeTokens(target);
        const auto header = TouchHeaderBackButton::headerRect(renderer, mappedInputManager);
        int top = 0, right = 0, bottom = 0, left = 0;
        renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
        const int y = header.y + header.height + 2 * metrics.verticalSpacing + top +
                      target.lineHeight(tokens.smallText.font) + tokens.rowHeight / 2;
        mappedInputManager.simulatorInjectTouchDown(renderer.getScreenWidth() / 2, y);
        mappedInputManager.simulatorInjectTouchRelease(renderer.getScreenWidth() / 2, y);
      } else
#endif
      {
        tap(Button::Confirm);
      }
      break;
    }
    case 14:
      expectCompleted(1);
      capture("/phase3-touch.bmp");
      activityManager.goToReader("/checklists/02-long.md");
      break;
    case 15:
      require(activityManager.isCurrentActivityNamed("Checklist"), "Long checklist did not open");
      capture("/phase3-long.bmp");
      break;
    case 16:
      tap(Button::Down);
      if (++scroll < 21) --step;
      break;
    case 17:
      capture("/phase3-last-task.bmp");
      tap(Button::Confirm);
      break;
    case 18: {
      MarkdownChecklist document("/checklists/02-long.md");
      require(document.load() == MarkdownChecklist::Status::Ok && document.taskCount() == 22 &&
                  document.row(document.rowCount() - 1).checked(),
              "Last task was not reachable");
      capture("/phase3-last-task-checked.bmp");
      activityManager.goToReader("/checklists/03-notes.md");
      break;
    }
    case 19:
      require(activityManager.isCurrentActivityNamed("TxtReader"), "Plain Markdown lost text reader fallback");
      activityManager.goToReader("/checklists/04-too-large.md");
      break;
    case 20:
      require(activityManager.isCurrentActivityNamed("Checklist"), "Limit screen missing");
      capture("/phase3-limit.bmp");
      tap(Button::Confirm);
      break;
    case 21:
      require(activityManager.isCurrentActivityNamed("TxtReader"), "View Markdown text did not open");
      activityManager.goHome();
      break;
    case 22:
      require(!activityManager.hasActivityNamed("Checklist"), "Checklist retained after leaving");
      activityManager.goHome(HomeMenuItem::CHECKLISTS);
      break;
    case 23: {
      require(activityManager.isHomeActivity(), "Home did not open");
      capture("/phase3-home.bmp");
#if CROSSINK_APP_CAP_TOUCH
      if (mappedInputManager.hasTouchHardware()) {
        const int menuIndex = UITheme::getInstance().getMetrics().homeContinueReadingInMenu ? 2 : 1;
        bool found = false;
        for (int y = 4; y < renderer.getScreenHeight() && !found; y += 8) {
          for (int x = 4; x < renderer.getScreenWidth() && !found; x += 8) {
            int id = -1;
            if (TouchRegistry::getInstance().hitTest(x, y, TouchRegistry::Item, id) && id == menuIndex) {
              mappedInputManager.simulatorInjectTouchDown(x, y);
              pendingTouchX = x;
              pendingTouchY = y;
              touchReleasePending = true;
              found = true;
            }
          }
        }
        require(found, "Home Checklists touch target missing");
      } else
#endif
      {
        tap(Button::Confirm);
      }
      break;
    }
    case 24:
      require(activityManager.isCurrentActivityNamed("Checklists"), "Home entry did not open Checklists");
      capture("/phase3-checklists-root.bmp");
      tap(Button::Back);
      break;
    case 25:
      require(activityManager.isHomeActivity(), "Checklists root Back did not return Home");
      activityManager.goToChecklists("/home-checklists");
      break;
    case 26:
      capture("/phase3-checklists-files.bmp");
      tap(Button::Confirm);
      break;
    case 27:
      require(activityManager.isCurrentActivityNamed("Checklist"), "Non-Markdown file was not filtered");
      tap(Button::Back);
      break;
    case 28:
      require(activityManager.isCurrentActivityNamed("Checklists"), "Checklist Back lost browser context");
      tap(Button::Up);  // wrap to uppercase .MD plain-text fixture
      break;
    case 29:
      tap(Button::Confirm);
      break;
    case 30:
      require(activityManager.isCurrentActivityNamed("TxtReader"), "Uppercase plain Markdown did not open");
      tap(Button::Back);
      break;
    case 31:
      require(activityManager.isCurrentActivityNamed("Checklists"), "Plain Markdown Back lost browser context");
      activityManager.goToReader("/home-checklists/01-preshow.md", false, false, false, true);
      break;
    case 32:
      tap(Button::Up);  // View Markdown text
      break;
    case 33:
      tap(Button::Confirm);
      break;
    case 34:
      require(activityManager.isCurrentActivityNamed("TxtReader"), "Source view did not open");
      tap(Button::Back);
      break;
    case 35:
      require(activityManager.isCurrentActivityNamed("Checklists"), "Source view Back lost browser context");
      activityManager.goToChecklists("/home-empty");
      break;
    case 36:
      capture("/phase3-checklists-empty.bmp");
      tap(Button::Back);
      break;
    case 37:
      require(activityManager.isCurrentActivityNamed("Checklists"), "Parent folder lost checklist filter");
      tap(Button::Back);
      break;
    case 38:
      require(activityManager.isHomeActivity(), "Folder navigation did not return Home");
      LOG_INF("CHECKTEST", "Phase 3 checklist smoke test passed");
      std::_Exit(0);
  }
  settle = 2;
  return true;
}
#endif
