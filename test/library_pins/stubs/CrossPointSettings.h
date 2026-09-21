#pragma once

#include <array>
#include <cstdint>
#include <string>

class CrossPointSettings {
 public:
  static constexpr uint8_t IGNORE = 0;
  static constexpr uint8_t OPEN_PINNED_DOC = 34;
  static constexpr uint8_t OPEN_PINNED_FOLDER = 35;

  std::string pinnedDocPath;
  std::string pinnedFolderPath;
  uint8_t quickActionSlots[5] = {};
  bool failSave = false;
  mutable unsigned saveCalls = 0;
  mutable std::string savedDocument;
  mutable std::string savedFolder;
  mutable std::array<uint8_t, 5> savedSlots{};

  bool saveToFile() const {
    ++saveCalls;
    if (failSave) return false;
    savedDocument = pinnedDocPath;
    savedFolder = pinnedFolderPath;
    for (size_t i = 0; i < savedSlots.size(); ++i) savedSlots[i] = quickActionSlots[i];
    return true;
  }
};
