#include "SyncedNote.h"

#include <HalStorage.h>
#include <Logging.h>

namespace SyncedNote {
namespace {
bool removeIfPresent(const char* path) {
  if (!Storage.exists(path) || Storage.remove(path)) return true;
  LOG_ERR("NOTE", "Could not remove %s", path);
  return false;
}
}  // namespace

bool recover() {
  if (Storage.exists(BACKUP_PATH)) {
    if (!Storage.exists(PATH)) {
      if (!Storage.rename(BACKUP_PATH, PATH)) {
        LOG_ERR("NOTE", "Could not restore previous note");
        return false;
      }
    } else if (!removeIfPresent(BACKUP_PATH)) {
      return false;
    }
  }
  return removeIfPresent(TEMP_PATH);
}

bool prepareUpload() {
  if (!Storage.ensureDirectoryExists(DIRECTORY)) {
    LOG_ERR("NOTE", "Could not create note directory");
    return false;
  }
  return recover();
}

bool finishUpload() {
  if (!Storage.exists(TEMP_PATH)) {
    LOG_ERR("NOTE", "Missing uploaded note");
    return false;
  }
  const bool hadOriginal = Storage.exists(PATH);
  if (hadOriginal && !Storage.rename(PATH, BACKUP_PATH)) {
    LOG_ERR("NOTE", "Could not back up previous note");
    return false;
  }
  if (!Storage.rename(TEMP_PATH, PATH)) {
    LOG_ERR("NOTE", "Could not publish uploaded note");
    if (hadOriginal && !Storage.rename(BACKUP_PATH, PATH)) {
      LOG_ERR("NOTE", "Previous note retained at %s; recovery needed", BACKUP_PATH);
    }
    return false;
  }
  // The complete new note is already published. Retry cleanup on next open.
  removeIfPresent(BACKUP_PATH);
  return true;
}

void discardUpload() { removeIfPresent(TEMP_PATH); }
}  // namespace SyncedNote
