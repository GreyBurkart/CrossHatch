#pragma once

#include <ArduinoJson.h>

#include <cstdint>
#include <string>
#include <string_view>

// Two persistent paths, not two live readers. Each string is bounded to 512 bytes
// and allocated only on assignment/load; their lifetime spans activity changes.
struct DocumentSlots {
  static constexpr size_t MAX_PATH_LENGTH = 512;
  std::string docSlotA;
  std::string docSlotB;
  uint8_t activeDocSlot = 0;

  static bool validDocumentPath(std::string_view path);
  bool hasDocSlotA() const { return !docSlotA.empty(); }
  bool hasDocSlotB() const { return !docSlotB.empty(); }
  bool assignDocumentSlot(const std::string& path, uint8_t slot);
  void noteOpenedDocument(const std::string& path);
  uint8_t targetDocumentSlot(const std::string& currentPath) const;
  // One bounded path copy on a user action; no reader/parser is retained here.
  std::string getTargetHopPath(const std::string& currentPath) const;
  void documentSlotsToJson(JsonDocument& doc) const;
  void documentSlotsFromJson(JsonVariantConst doc);
};
