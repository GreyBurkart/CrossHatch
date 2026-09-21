#ifdef SIMULATOR

#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>

#include <array>
#include <cstdlib>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "GlobalActions.h"
#include "QuickActions.h"
#include "RecentBooksStore.h"
#include "activities/ActivityManager.h"
#include "activities/home/BookActions.h"
#include "activities/home/VirtualViews.h"
#include "util/ScreenshotUtil.h"

extern ActivityManager activityManager;
extern GfxRenderer renderer;
extern MappedInputManager mappedInputManager;

namespace {
constexpr const char* BOOK_A = "/books/main.epub";
constexpr const char* BOOK_B = "/books/reference.txt";

void require(const bool condition, const char* message) {
  if (condition) return;
  LOG_ERR("LIBTEST", "%s", message);
  std::_Exit(2);
}

std::string progressPath(const bool epub) {
  return (epub ? Epub::cachePathForFilePath(BOOK_A, "/.crosspoint")
               : "/.crosspoint/txt_" + std::to_string(std::hash<std::string>{}(BOOK_B))) +
         "/progress.bin";
}

std::array<uint8_t, 12> position(const bool epub) {
  require(activityManager.prepareForDocumentSwitch(), "Progress flush failed");
  std::array<uint8_t, 12> bytes{};
  auto file = Storage.open(progressPath(epub).c_str());
  require(static_cast<bool>(file), "Progress file missing");
  const size_t size = file.size();
  require(size > 0 && size < bytes.size(), "Unexpected progress file size");
  require(file.read(bytes.data(), size) == static_cast<int>(size), "Could not read progress");
  bytes.back() = static_cast<uint8_t>(size);
  file.close();
  return bytes;
}

void capture(const char* path) {
  require(activityManager.requestUpdateAndWait() == RequestUpdateResult::Rendered, "Render rejected");
  RenderLock lock;
  require(ScreenshotUtil::saveFramebufferAsBmp(path, renderer.getFrameBuffer(), renderer.getDisplayWidth(),
                                               renderer.getDisplayHeight()),
          "Screenshot failed");
}
}  // namespace

// Isolated, explicitly enabled integration test: exercises production actions,
// actual EPUB/TXT readers, SD persistence, and foreground view rendering.
bool runSimulatorLibrarySmokeTestTick() {
  if (!std::getenv("CROSSHATCH_PHASE2_SMOKE")) return false;
  static unsigned step = 0;
  static unsigned settle = 0;
  static bool releasePageButton = false;
  static unsigned long powerReleaseAt = 0;
  static bool heldPowerOpenedB = false;
  static std::array<uint8_t, 12> positionA{};
  static std::array<uint8_t, 12> positionB{};
  mappedInputManager.simulatorClearInputFrame();
  if (releasePageButton) {
    mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::PageForward);
    releasePageButton = false;
    return true;
  }
  if (settle) {
    --settle;
    // TXT initializes its page index on the render task. Wait for the actual
    // frame before injecting input; elapsed loop ticks alone race under load.
    if (!settle) require(activityManager.requestUpdateAndWait() == RequestUpdateResult::Rendered, "Render rejected");
    return true;
  }
  LOG_INF("LIBTEST", "Step %u", step);
  switch (step++) {
    case 0: {
      require(Storage.exists(BOOK_A) && Storage.exists(BOOK_B), "Missing isolated fixtures");
      StrId feedback{};
      require(BookActions::handleLibraryAction(FileBrowserAction::AssignDocA, BOOK_A, feedback), "Assign A failed");
      require(BookActions::handleLibraryAction(FileBrowserAction::AssignDocB, BOOK_B, feedback), "Assign B failed");
      require(APP_STATE.docSlotA == BOOK_A && APP_STATE.docSlotB == BOOK_B, "Assignments not retained");
      BookActions::handleLibraryAction(FileBrowserAction::PinDocument, BOOK_A, feedback);
      BookActions::handleLibraryAction(FileBrowserAction::PinFolder, "/books", feedback);
      require(SETTINGS.pinnedDocPath == BOOK_A && SETTINGS.pinnedFolderPath == "/books", "Pins not retained");
      SETTINGS.quickActionSlots[2] = CrossPointSettings::AB_DOCUMENT_HOP;
      SETTINGS.quickActionSlots[3] = CrossPointSettings::VIEW_RECENTLY_ADDED;
      SETTINGS.quickActionSlots[4] = CrossPointSettings::VIEW_RECENTLY_FINISHED;
      require(SETTINGS.saveToFile() && SETTINGS.loadFromFile(), "Settings round trip failed");
      require(SETTINGS.quickActionSlots[2] == CrossPointSettings::AB_DOCUMENT_HOP &&
                  SETTINGS.quickActionSlots[4] == CrossPointSettings::VIEW_RECENTLY_FINISHED &&
                  SETTINGS.pinnedDocPath == BOOK_A,
              "New settings did not survive reload");
      require(APP_STATE.loadFromFile() && APP_STATE.docSlotB == BOOK_B, "Slots did not survive reload");
      require(activityManager.goToReader(BOOK_A), "Open A rejected");
      break;
    }
    case 1:
      require(activityManager.getCurrentBookPath() == BOOK_A, "A did not open");
      require(dispatchShortcutAction(CrossPointSettings::PAGE_TURN), "A page turn rejected");
      break;
    case 2:
      positionA = position(true);
      require(positionA[2] != 0 || positionA[0] != 0, "A never advanced");
      require(dispatchShortcutAction(CrossPointSettings::AB_DOCUMENT_HOP), "A/B action rejected");
      break;
    case 3:
      require(activityManager.getCurrentBookPath() == BOOK_B && APP_STATE.activeDocSlot == 1, "B did not open");
      mappedInputManager.simulatorInjectPress(MappedInputManager::Button::PageForward);
      releasePageButton = true;
      break;
    case 4:
      positionB = position(false);
      require(positionB[0] != 0, "B never advanced");
      dispatchShortcutAction(CrossPointSettings::AB_DOCUMENT_HOP);
      break;
    case 5:
      require(activityManager.getCurrentBookPath() == BOOK_A && APP_STATE.activeDocSlot == 0, "A did not reopen");
      require(position(true) == positionA, "A position changed after hop");
      require(APP_STATE.assignCurrentToSlot("/books/missing.txt", 1), "Missing target setup failed");
      dispatchShortcutAction(CrossPointSettings::AB_DOCUMENT_HOP);
      break;
    case 6:
      require(activityManager.getCurrentBookPath() == BOOK_A, "Missing target closed A");
      require(APP_STATE.assignCurrentToSlot(BOOK_B, 1), "Restore B failed");
      dispatchShortcutAction(CrossPointSettings::AB_DOCUMENT_HOP);
      break;
    case 7:
      require(position(false) == positionB, "B position changed after hop");
      dispatchShortcutAction(CrossPointSettings::OPEN_PINNED_FOLDER);
      break;
    case 8:
      require(activityManager.isCurrentActivityNamed("FileBrowser"), "Pinned folder did not open");
      dispatchShortcutAction(CrossPointSettings::OPEN_PINNED_DOC);
      break;
    case 9:
      require(activityManager.getCurrentBookPath() == BOOK_A && position(true) == positionA,
              "Pinned document lost position");
      dispatchShortcutAction(CrossPointSettings::VIEW_RECENTLY_ADDED);
      break;
    case 10: {
      require(activityManager.isCurrentActivityNamed("RecentBooks"), "Added view did not open");
      capture("/phase2-added.bmp");
      bool completed = false;
      require(BookActions::toggleBookCompleted(BOOK_A, "Main", completed) && completed, "Mark finished failed");
      RECENT_BOOKS.removeByPath(BOOK_A);
      std::vector<RecentBook> books;
      const auto status = VirtualViews::loadRecentlyFinished(books);
      require(!status.failed && books.size() == 1 && books.front().path == BOOK_A,
              "Finished view lost book removed from recents");
      dispatchShortcutAction(CrossPointSettings::VIEW_RECENTLY_FINISHED);
      break;
    }
    case 11:
      capture("/phase2-finished.bmp");
      dispatchShortcutAction(CrossPointSettings::VIEW_RECENTLY_OPENED);
      break;
    case 12:
      capture("/phase2-opened.bmp");
      dispatchShortcutAction(CrossPointSettings::OPEN_PINNED_DOC);
      break;
    case 13:
      require(activityManager.isReaderActivity(), "Home regression setup did not open A");
      activityManager.goHome();
      break;
    case 14:
      require(!activityManager.isReaderActivity() && activityManager.getCurrentBookPath() == BOOK_A,
              "Home regression setup did not highlight A");
      dispatchShortcutAction(CrossPointSettings::OPEN_PINNED_DOC);
      break;
    case 15:
      require(activityManager.isReaderActivity() && activityManager.getCurrentBookPath() == BOOK_A &&
                  position(true) == positionA,
              "Pinned document did not open from Home");
      SETTINGS.shortPwrBtn = CrossPointSettings::IGNORE;
      SETTINGS.longPwrBtn = CrossPointSettings::AB_DOCUMENT_HOP;
      powerReleaseAt = millis() + SETTINGS.getPowerButtonLongPressDuration() + 800;
      mappedInputManager.simulatorInjectPress(MappedInputManager::Button::Power);
      break;
    case 16: {
      const std::string current = activityManager.getCurrentBookPath();
      if (current == BOOK_B) heldPowerOpenedB = true;
      if (heldPowerOpenedB) require(current == BOOK_B, "Held Power switched more than once");
      if (millis() < powerReleaseAt) {
        --step;
        return true;
      }
      require(heldPowerOpenedB, "Long Power did not switch documents");
      mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Power);
      break;
    }
    case 17:
      require(activityManager.getCurrentBookPath() == BOOK_B, "Power release repeated the switch");
      dispatchShortcutAction(CrossPointSettings::AB_DOCUMENT_HOP);
      break;
    case 18:
      require(activityManager.getCurrentBookPath() == BOOK_A && position(true) == positionA,
              "Progress-failure setup did not restore A");
      // A single rendered turn remains dirty in the production debouncer.
      require(dispatchShortcutAction(CrossPointSettings::PREVIOUS_PAGE), "Fault setup page turn rejected");
      break;
    case 19: {
      const std::string progress = progressPath(true);
      const std::string backup = progress + ".fault-backup";
      const std::string blockedTemp = progress + ".tmp";
      {
        RenderLock lock;
        require(!Storage.exists(backup.c_str()) && !Storage.exists(blockedTemp.c_str()), "Fault paths already exist");
        require(Storage.rename(progress.c_str(), backup.c_str()), "Could not preserve progress for fault test");
        // The EPUB writer rotates progress.bin, so blocking that name alone
        // would succeed. A nonempty temp directory rejects its first cleanup.
        require(Storage.mkdir(blockedTemp.c_str()), "Could not create progress write blocker");
        HalFile marker;
        require(Storage.openFileForWrite("LIBTEST", blockedTemp + "/blocker", marker),
                "Could not create blocker marker");
        require(marker.write(static_cast<uint8_t>(1)) == 1, "Could not write blocker marker");
        marker.close();
      }
      // This must reach the production progress guard and remain on A.
      require(dispatchShortcutAction(CrossPointSettings::AB_DOCUMENT_HOP), "Faulted hop action rejected");
      break;
    }
    case 20: {
      require(activityManager.isReaderActivity() && activityManager.getCurrentBookPath() == BOOK_A &&
                  APP_STATE.activeDocSlot == 0,
              "Failed progress save closed A");
      const std::string progress = progressPath(true);
      const std::string blockedTemp = progress + ".tmp";
      {
        RenderLock lock;
        require(Storage.remove((blockedTemp + "/blocker").c_str()), "Could not remove blocker marker");
        require(Storage.rmdir(blockedTemp.c_str()), "Could not remove progress write blocker");
        require(Storage.rename((progress + ".fault-backup").c_str(), progress.c_str()), "Could not restore progress");
      }
      require(position(true) != positionA, "Fault setup did not dirty A's reading position");
      require(dispatchShortcutAction(CrossPointSettings::PAGE_TURN), "Recovery page turn rejected");
      break;
    }
    case 21:
      require(position(true) == positionA, "Recovery did not restore A's original position");
      require(dispatchShortcutAction(CrossPointSettings::AB_DOCUMENT_HOP), "Recovered hop rejected");
      break;
    case 22:
      require(activityManager.getCurrentBookPath() == BOOK_B && position(false) == positionB,
              "Recovery did not reopen B at its saved position");
      require(dispatchShortcutAction(CrossPointSettings::AB_DOCUMENT_HOP), "Recovery return to A rejected");
      break;
    case 23:
      require(activityManager.getCurrentBookPath() == BOOK_A && position(true) == positionA,
              "Recovery return lost A's position");
      require(dispatchShortcutAction(CrossPointSettings::QUICK_ACTIONS), "Quick Actions did not open");
      break;
    case 24:
      capture("/phase2-actions.bmp");
      LOG_INF("LIBTEST", "Phase 2 library smoke test passed");
      std::_Exit(0);
  }
  settle = 12;
  return true;
}
#endif
