#include "LibraryPins.h"

#include <Logging.h>

#include <cstring>

#include "CrossPointSettings.h"

bool LibraryPins::validPath(const std::string_view path) {
  if (path.empty() || path.front() != '/' || path.size() > MAX_PATH_LENGTH || path.find('\0') != path.npos) {
    return false;
  }
  if (path.size() > 1 && path.back() == '/') return false;
  size_t start = 1;
  while (start < path.size()) {
    const size_t end = path.find('/', start);
    const auto component = path.substr(start, end == path.npos ? path.size() - start : end - start);
    if (component.empty() || component == "." || component == "..") return false;
    if (end == path.npos) break;
    start = end + 1;
  }
  return true;
}

bool LibraryPins::set(CrossPointSettings& settings, const std::string& path, const bool directory, const bool pin) {
  auto& target = directory ? settings.pinnedFolderPath : settings.pinnedDocPath;
  if (!pin && target != path) return true;
  if (pin && !validPath(path)) {
    LOG_ERR("LibraryPins", "Invalid pin path: %s", path.c_str());
    return false;
  }
  const uint8_t action = directory ? CrossPointSettings::OPEN_PINNED_FOLDER : CrossPointSettings::OPEN_PINNED_DOC;
  // One cold-path copy, at most 1024 bytes, preserves the previous owned path
  // for rollback without putting a path-sized buffer on the activity stack.
  std::string previousPath = pin ? path : std::string();
  uint8_t previousSlots[sizeof(settings.quickActionSlots)];
  memcpy(previousSlots, settings.quickActionSlots, sizeof(previousSlots));
  target.swap(previousPath);
  if (pin) {
    bool alreadyAssigned = false;
    for (const uint8_t slot : settings.quickActionSlots) {
      alreadyAssigned |= slot == action;
    }
    if (!alreadyAssigned) {
      for (uint8_t& slot : settings.quickActionSlots) {
        if (slot == CrossPointSettings::IGNORE) {
          slot = action;
          break;
        }
      }
    }
  } else {
    for (uint8_t& slot : settings.quickActionSlots) {
      if (slot == action) slot = CrossPointSettings::IGNORE;
    }
  }
  if (!settings.saveToFile()) {
    target.swap(previousPath);
    memcpy(settings.quickActionSlots, previousSlots, sizeof(previousSlots));
    LOG_ERR("LibraryPins", "Failed to save pin: %s", path.c_str());
    return false;
  }
  return true;
}
