#include "BookActions.h"

#include <Epub.h>
#include <Epub/EpubRenderMode.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <TrashPaths.h>
#include <Xtc.h>

#include <cstdio>
#include <cstring>

#include "BookmarkStore.h"
#include "ClippingStore.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "RecentBooksStore.h"
#include "activities/boot_sleep/ImageFolderIndex.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/EpubReaderActivity.h"
#include "activities/reader/GlobalReadingStats.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#include "util/BookMoveUtils.h"
#include "util/LibraryPins.h"

namespace BookActions {
namespace {

bool hasReadingStats(const std::string& path) {
  return FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path);
}

bool isReaderDocument(const std::string& path) {
  return FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path) || FsHelpers::hasTxtExtension(path) ||
         FsHelpers::hasMarkdownExtension(path);
}

bool validLibraryTarget(const std::string& path, const bool directory) {
  if (!LibraryPins::validPath(path) || crosshatch::trash::isPath(path.c_str()) ||
      (!directory && !isReaderDocument(path))) {
    LOG_ERR("BookActions", "Invalid library target: %s", path.c_str());
    return false;
  }
  auto file = Storage.open(path.c_str());
  const bool valid = file && file.isDirectory() == directory;
  file.close();
  if (!valid) {
    LOG_ERR("BookActions", "Library target unavailable: %s", path.c_str());
  }
  return valid;
}

std::string bookStatsCachePath(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) {
    return Epub(path, "/.crosspoint").getCachePath();
  }
  if (FsHelpers::hasXtcExtension(path)) {
    return Xtc(path, "/.crosspoint").getCachePath();
  }
  return "";
}

}  // namespace

std::vector<FileBrowserActionActivity::MenuItem> buildBookActionItems(const std::string& fullPath,
                                                                      const bool includeRemoveFromRecents) {
  std::vector<FileBrowserActionActivity::MenuItem> items;
  // At most 12 small menu entries, retained by the popup across render calls.
  items.reserve(12);
  if (crosshatch::trash::isPath(fullPath.c_str())) {
    items.push_back({FileBrowserAction::Restore, StrId::STR_RESTORE});
    items.push_back({FileBrowserAction::Delete, StrId::STR_PERMANENT_DELETE});
    return items;
  }
  if (isReaderDocument(fullPath)) {
    items.push_back({FileBrowserAction::AssignDocA, StrId::STR_SET_DOC_A});
    items.push_back({FileBrowserAction::AssignDocB, StrId::STR_SET_DOC_B});
    const bool pinned = SETTINGS.pinnedDocPath == fullPath;
    items.push_back({pinned ? FileBrowserAction::UnpinDocument : FileBrowserAction::PinDocument,
                     pinned ? StrId::STR_UNPIN_DOCUMENT : StrId::STR_PIN_DOCUMENT});
  }
  const StrId deleteLabel = (SETTINGS.recycleBinEnabled != 0) ? StrId::STR_MOVE_TO_TRASH : StrId::STR_DELETE;
  items.push_back({FileBrowserAction::Delete, deleteLabel});
  if (hasClearableBookCache(fullPath)) {
    items.push_back({FileBrowserAction::DeleteCache, StrId::STR_DELETE_CACHE});
  }
  if (FsHelpers::hasEpubExtension(fullPath)) {
    items.push_back({FileBrowserAction::EpubRenderMode, StrId::STR_EPUB_RENDER_MODE});
    items.push_back({FileBrowserAction::ResetReaderSettings, StrId::STR_RESET_BOOK_READER_SETTINGS});
  }
  if (hasReadingStats(fullPath)) {
    items.push_back({FileBrowserAction::DeleteStats, StrId::STR_DELETE_BOOK_STATS});
    items.push_back({FileBrowserAction::ToggleCompleted,
                     isBookCompleted(fullPath) ? StrId::STR_MARK_UNFINISHED : StrId::STR_MARK_FINISHED});
  }
  if (includeRemoveFromRecents) {
    items.push_back({FileBrowserAction::RemoveFromRecents, StrId::STR_REMOVE_FROM_RECENTS_ACTION});
  }
  return items;
}

bool handleLibraryAction(const FileBrowserAction action, const std::string& fullPath, StrId& feedback) {
  bool directory = false;
  bool pin = false;
  switch (action) {
    case FileBrowserAction::AssignDocA:
    case FileBrowserAction::AssignDocB:
      feedback = StrId::STR_LIBRARY_TARGET_MISSING;
      if (validLibraryTarget(fullPath, false)) {
        const bool slotB = action == FileBrowserAction::AssignDocB;
        feedback = APP_STATE.assignCurrentToSlot(fullPath, slotB ? 1 : 0)
                       ? (slotB ? StrId::STR_SET_DOC_B : StrId::STR_SET_DOC_A)
                       : StrId::STR_LIBRARY_SAVE_FAILED;
      }
      return true;
    case FileBrowserAction::PinDocument:
      pin = true;
      break;
    case FileBrowserAction::PinFolder:
      directory = true;
      pin = true;
      break;
    case FileBrowserAction::UnpinDocument:
      break;
    case FileBrowserAction::UnpinFolder:
      directory = true;
      break;
    default:
      return false;
  }
  feedback = StrId::STR_LIBRARY_TARGET_MISSING;
  if (pin && !validLibraryTarget(fullPath, directory)) return true;
  if (!LibraryPins::set(SETTINGS, fullPath, directory, pin)) {
    feedback = StrId::STR_LIBRARY_SAVE_FAILED;
  } else if (directory) {
    feedback = pin ? StrId::STR_FOLDER_PINNED : StrId::STR_FOLDER_UNPINNED;
  } else {
    feedback = pin ? StrId::STR_DOCUMENT_PINNED : StrId::STR_DOCUMENT_UNPINNED;
  }
  if (pin && feedback != StrId::STR_LIBRARY_SAVE_FAILED) {
    const uint8_t pinnedAction =
        directory ? CrossPointSettings::OPEN_PINNED_FOLDER : CrossPointSettings::OPEN_PINNED_DOC;
    bool inPopup = false;
    for (const uint8_t slot : SETTINGS.quickActionSlots) inPopup |= slot == pinnedAction;
    if (!inPopup) feedback = StrId::STR_PINNED_CHOOSE_SLOT;
  }
  return true;
}

bool hasClearableBookCache(const std::string& path) {
  return FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path);
}

bool canSendNearby(const std::string& path) {
  return FsHelpers::hasEpubExtension(path) || FsHelpers::hasTxtExtension(path) || FsHelpers::hasXtcExtension(path) ||
         FsHelpers::hasPngExtension(path) || FsHelpers::hasBmpExtension(path);
}

void clearFileMetadata(const std::string& fullPath) {
  if (FsHelpers::hasEpubExtension(fullPath)) {
    Epub(fullPath, "/.crosspoint").clearCache();
    BookmarkStore::deleteForFilePath(fullPath, "epub");
    ClippingStore::deleteForFilePath(fullPath, "epub");
  } else if (FsHelpers::hasXtcExtension(fullPath)) {
    BookmarkStore::deleteForFilePath(fullPath, "xtc");
  } else if (FsHelpers::hasTxtExtension(fullPath) || FsHelpers::hasMarkdownExtension(fullPath)) {
    BookmarkStore::deleteForFilePath(fullPath, "txt");
  }
}

bool clearBookCache(const std::string& fullPath) {
  if (FsHelpers::hasEpubExtension(fullPath) || FsHelpers::hasXtcExtension(fullPath)) {
    return clearBookCachePreservingUserState(fullPath);
  }
  return false;
}

bool deleteBookStats(const std::string& fullPath) {
  const std::string cachePath = bookStatsCachePath(fullPath);
  if (cachePath.empty()) {
    return false;
  }
  return BookReadingStats::remove(cachePath);
}

bool resetBookReaderSettings(const std::string& fullPath) {
  if (!FsHelpers::hasEpubExtension(fullPath)) {
    return false;
  }
  return EpubReaderActivity::resetBookReaderSettings(fullPath);
}

std::vector<std::string> epubRenderModeOptions() {
  return {I18N.get(StrId::STR_RENDER_MODE_CROSSINK_DEFAULT), I18N.get(StrId::STR_RENDER_MODE_BALANCED),
          I18N.get(StrId::STR_RENDER_MODE_LIGHT)};
}

uint8_t epubRenderModeDisplayIndex(const uint8_t renderMode) {
  switch (static_cast<EpubRenderMode>(renderMode)) {
    case EpubRenderMode::Balanced:
      return 1;
    case EpubRenderMode::Light:
      return 2;
    case EpubRenderMode::CrossInkDefault:
    default:
      return 0;
  }
}

uint8_t epubRenderModeForDisplayIndex(const uint8_t displayIndex) {
  switch (displayIndex) {
    case 1:
      return static_cast<uint8_t>(EpubRenderMode::Balanced);
    case 2:
      return static_cast<uint8_t>(EpubRenderMode::Light);
    case 0:
    default:
      return static_cast<uint8_t>(EpubRenderMode::CrossInkDefault);
  }
}

std::string confirmationHeading(const StrId actionLabelId) {
  return std::string(tr(STR_CONFIRM)) + ": " + std::string(I18N.get(actionLabelId));
}

bool isBookCompleted(const std::string& fullPath) {
  const std::string cachePath = bookStatsCachePath(fullPath);
  return !cachePath.empty() && BookReadingStats::load(cachePath).isCompleted;
}

bool toggleBookCompleted(const std::string& fullPath, const std::string& displayName, bool& completed) {
  const bool isEpub = FsHelpers::hasEpubExtension(fullPath);
  const bool isXtc = FsHelpers::hasXtcExtension(fullPath);
  if (!isEpub && !isXtc) {
    return false;
  }

  Epub epub(fullPath, "/.crosspoint");
  Xtc xtc(fullPath, "/.crosspoint");
  std::string cachePath;
  std::string title;
  std::string author;
  std::string thumbPath;
  if (isEpub) {
    epub.setupCacheDir();
    cachePath = epub.getCachePath();
    title = epub.getTitle();
    author = epub.getAuthor();
    thumbPath = epub.getThumbBmpPath();
  } else {
    if (!xtc.load()) {
      return false;
    }
    xtc.setupCacheDir();
    cachePath = xtc.getCachePath();
    title = xtc.getTitle();
    author = xtc.getAuthor();
    thumbPath = xtc.getThumbBmpPath();
  }

  BookReadingStats stats = BookReadingStats::load(cachePath);
  completed = !stats.isCompleted;
  stats.isCompleted = completed;
  if (completed && !stats.finishedDateManual) {
    ReadingStatsDateTime now;
    if (getCurrentLocalReadingStatsDateTime(now)) {
      stats.finishedDate = now.date;
    }
  }

  GlobalReadingStats globalStats = GlobalReadingStats::load();
  if (completed) {
    globalStats.completedBooks++;
  } else if (globalStats.completedBooks > 0) {
    globalStats.completedBooks--;
  }

  stats.save(cachePath);
  globalStats.save();

  if (SETTINGS.removeReadBooksFromRecents) {
    if (completed) {
      RECENT_BOOKS.removeByPath(fullPath);
    } else {
      RECENT_BOOKS.addOrUpdateBook(fullPath, title, author, thumbPath);
    }
  }

  if (isEpub && completed && SETTINGS.moveFinishedToReadFolder && fullPath.rfind("/Read/", 0) != 0) {
    const std::string oldCachePath = epub.getCachePath();
    const std::string dstPath = BookMoveUtils::buildReadFolderDestination(fullPath);
    LOG_INF("BookActions", "Moving completed epub: %s -> %s", fullPath.c_str(), dstPath.c_str());
    if (!Storage.rename(fullPath.c_str(), dstPath.c_str())) {
      LOG_ERR("BookActions", "Failed to move book to 'Read' folder");
      snprintf(APP_STATE.pendingAlertTitle, sizeof(APP_STATE.pendingAlertTitle), "%s",
               tr(STR_MOVE_TO_READ_FAILED_TITLE));
      snprintf(APP_STATE.pendingAlertBody, sizeof(APP_STATE.pendingAlertBody), tr(STR_MOVE_TO_READ_FAILED_BODY),
               displayName.c_str());
      APP_STATE.pendingAlertGoHomeOnBack.store(false, std::memory_order_relaxed);
      APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
      return true;
    }

    BookMoveUtils::migrateMovedEpubState(fullPath, dstPath, oldCachePath, title, author,
                                         !SETTINGS.removeReadBooksFromRecents);
  }

  return true;
}

void drawToast(const GfxRenderer& renderer, const char* msg) {
  constexpr int toastPadX = 20;
  constexpr int toastPadY = 12;
  const int msgW = renderer.getTextWidth(UI_10_FONT_ID, msg);
  const int msgH = renderer.getLineHeight(UI_10_FONT_ID);
  const int toastW = msgW + toastPadX * 2;
  const int toastH = msgH + toastPadY * 2;
  const int toastX = (renderer.getScreenWidth() - toastW) / 2;
  const int toastY = (renderer.getScreenHeight() - toastH) / 2;
  renderer.fillRect(toastX, toastY, toastW, toastH, true);
  renderer.drawText(UI_10_FONT_ID, toastX + toastPadX, toastY + toastPadY, msg, false);
  renderer.displayBuffer();
}

bool deleteOrTrashFile(const std::string& fullPath, bool& movedToTrash) {
  if (SETTINGS.recycleBinEnabled && !crosshatch::trash::isPath(fullPath.c_str())) {
    char trashParent[256];
    if (!crosshatch::trash::buildTrashParent(trashParent, sizeof(trashParent), fullPath.c_str())) {
      LOG_ERR("BookActions", "Failed to build trash parent for: %s", fullPath.c_str());
      return false;
    }
    if (!Storage.exists(trashParent) && !Storage.mkdir(trashParent)) {
      LOG_ERR("BookActions", "Failed to create trash parent: %s", trashParent);
      return false;
    }
    const char* lastSlash = strrchr(fullPath.c_str(), '/');
    const char* fileName = lastSlash ? lastSlash + 1 : fullPath.c_str();
    char trashDest[256];
    const bool found = crosshatch::trash::findVacantPath(
        trashDest, sizeof(trashDest), trashParent, fileName, [](const char* candidate) {
          return Storage.exists(candidate) ? crosshatch::trash::PathProbe::Occupied
                                           : crosshatch::trash::PathProbe::Vacant;
        });
    if (!found) {
      LOG_ERR("BookActions", "Failed to find vacant trash path for: %s", fullPath.c_str());
      return false;
    }
    if (!Storage.rename(fullPath.c_str(), trashDest)) {
      LOG_ERR("BookActions", "Failed to move file to trash: %s -> %s", fullPath.c_str(), trashDest);
      return false;
    }
    movedToTrash = true;
  } else {
    clearFileMetadata(fullPath);
    if (crosshatch::trash::isPath(fullPath.c_str())) {
      const char* origPath = fullPath.c_str() + strlen(crosshatch::trash::DIRECTORY);
      clearFileMetadata(origPath);
    }
    if (!Storage.remove(fullPath.c_str())) {
      LOG_ERR("BookActions", "Failed to delete file: %s", fullPath.c_str());
      return false;
    }
    movedToTrash = false;
  }

  RECENT_BOOKS.removeByPath(fullPath);
  ImageFolderIndex::invalidateForPath(fullPath.c_str());

  if (APP_STATE.favoriteSleepImagePath == fullPath) {
    APP_STATE.favoriteSleepImagePath.clear();
    APP_STATE.saveToFile();
  }
  if (APP_STATE.favoriteBootImagePath == fullPath) {
    APP_STATE.favoriteBootImagePath.clear();
    APP_STATE.saveToFile();
  }
  return true;
}

bool restoreTrashedFile(const std::string& fullPath, std::string& restoredPath) {
  if (!crosshatch::trash::isPath(fullPath.c_str())) {
    return false;
  }

  char parentDir[256];
  char destPath[256];
  const char* lastSlash = strrchr(fullPath.c_str(), '/');
  const char* fileName = lastSlash ? lastSlash + 1 : fullPath.c_str();

  const bool targetFound = crosshatch::trash::findRestorePath(
      destPath, sizeof(destPath), parentDir, sizeof(parentDir), fullPath.c_str(), fileName,
      [](const char* parent) {
        if (strcmp(parent, "/") == 0) return true;
        return Storage.exists(parent) || Storage.mkdir(parent);
      },
      [](const char* candidate) {
        return Storage.exists(candidate) ? crosshatch::trash::PathProbe::Occupied
                                         : crosshatch::trash::PathProbe::Vacant;
      });

  if (!targetFound) {
    LOG_ERR("BookActions", "Failed to find restore path for: %s", fullPath.c_str());
    return false;
  }

  if (!Storage.rename(fullPath.c_str(), destPath)) {
    LOG_ERR("BookActions", "Failed to rename during restore: %s -> %s", fullPath.c_str(), destPath);
    return false;
  }

  restoredPath = destPath;
  return true;
}

}  // namespace BookActions
