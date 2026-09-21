#include "DocumentSlots.h"

namespace {
bool endsWithIgnoreCase(const std::string_view value, const std::string_view suffix) {
  if (value.size() < suffix.size()) return false;
  const auto tail = value.substr(value.size() - suffix.size());
  for (size_t i = 0; i < suffix.size(); ++i) {
    const char c = tail[i] >= 'A' && tail[i] <= 'Z' ? tail[i] + ('a' - 'A') : tail[i];
    if (c != suffix[i]) return false;
  }
  return true;
}
}  // namespace

bool DocumentSlots::validDocumentPath(const std::string_view path) {
  if (path.empty() || path.size() > MAX_PATH_LENGTH || path.front() != '/' ||
      path.find('\0') != std::string_view::npos || path.find("//") != std::string_view::npos ||
      path.find("/../") != std::string_view::npos || path.find("/./") != std::string_view::npos) {
    return false;
  }
  return endsWithIgnoreCase(path, ".epub") || endsWithIgnoreCase(path, ".txt") || endsWithIgnoreCase(path, ".xtc") ||
         endsWithIgnoreCase(path, ".xtch") || endsWithIgnoreCase(path, ".md");
}

bool DocumentSlots::assignDocumentSlot(const std::string& path, const uint8_t slot) {
  if (slot > 1 || !validDocumentPath(path)) return false;
  auto& selected = slot == 0 ? docSlotA : docSlotB;
  auto& other = slot == 0 ? docSlotB : docSlotA;
  // Assign first: path may alias the slot that is about to be cleared.
  selected = path;
  if (other == selected) other.clear();
  return true;
}

void DocumentSlots::noteOpenedDocument(const std::string& path) {
  if (!path.empty() && path == docSlotA)
    activeDocSlot = 0;
  else if (!path.empty() && path == docSlotB)
    activeDocSlot = 1;
}

uint8_t DocumentSlots::targetDocumentSlot(const std::string& currentPath) const {
  if (!currentPath.empty() && currentPath == docSlotA) return 1;
  if (!currentPath.empty() && currentPath == docSlotB) return 0;
  // Reading a third book must not silently replace an explicitly chosen slot.
  // With no slot currently open, return to the last successfully opened slot.
  if (!hasDocSlotA() && hasDocSlotB()) return 1;
  if (!hasDocSlotB() && hasDocSlotA()) return 0;
  return activeDocSlot == 1 ? 1 : 0;
}

std::string DocumentSlots::getTargetHopPath(const std::string& currentPath) const {
  return targetDocumentSlot(currentPath) == 0 ? docSlotA : docSlotB;
}

void DocumentSlots::documentSlotsToJson(JsonDocument& doc) const {
  doc["docSlotA"] = docSlotA;
  doc["docSlotB"] = docSlotB;
  doc["activeDocSlot"] = activeDocSlot;
}

void DocumentSlots::documentSlotsFromJson(JsonVariantConst doc) {
  docSlotA.clear();
  docSlotB.clear();
  const JsonString pathA = doc["docSlotA"].as<JsonString>();
  const JsonString pathB = doc["docSlotB"].as<JsonString>();
  // Validate before copying so malformed persisted paths cannot grow the slots.
  if (pathA && validDocumentPath({pathA.c_str(), pathA.size()})) docSlotA.assign(pathA.c_str(), pathA.size());
  if (pathB && validDocumentPath({pathB.c_str(), pathB.size()})) docSlotB.assign(pathB.c_str(), pathB.size());
  if (!docSlotA.empty() && docSlotA == docSlotB) docSlotB.clear();
  activeDocSlot = doc["activeDocSlot"].is<unsigned>() && doc["activeDocSlot"].as<unsigned>() == 1 ? 1 : 0;
  if (activeDocSlot == 1 && docSlotB.empty()) activeDocSlot = 0;
  if (activeDocSlot == 0 && docSlotA.empty() && !docSlotB.empty()) activeDocSlot = 1;
}
