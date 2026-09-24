#pragma once

#include <string>
#include <vector>

#include "FileBrowserActionActivity.h"

class GfxRenderer;

namespace BookActions {

std::vector<FileBrowserActionActivity::MenuItem> buildBookActionItems(const std::string& fullPath,
                                                                      bool includeRemoveFromRecents);
// Returns true for library actions, with success or failure text in feedback.
// Saves assignments and pins before reporting success; does not render.
bool handleLibraryAction(FileBrowserAction action, const std::string& fullPath, StrId& feedback);
bool hasClearableBookCache(const std::string& path);
bool canSendNearby(const std::string& path);
void clearFileMetadata(const std::string& fullPath);
bool clearBookCache(const std::string& fullPath);
bool deleteBookStats(const std::string& fullPath);
bool resetBookReaderSettings(const std::string& fullPath);
std::vector<std::string> epubRenderModeOptions();
uint8_t epubRenderModeDisplayIndex(uint8_t renderMode);
uint8_t epubRenderModeForDisplayIndex(uint8_t displayIndex);
std::string confirmationHeading(StrId actionLabelId);
bool isBookCompleted(const std::string& fullPath);
bool toggleBookCompleted(const std::string& fullPath, const std::string& displayName, bool& completed);
bool isBookPinnedToHome(const std::string& fullPath);
// Pins fullPath as the single Home-pinned book (replacing any previous pin), or unpins it if it is already the pin.
bool togglePinnedToHome(const std::string& fullPath, bool& pinned);
void drawToast(const GfxRenderer& renderer, const char* msg);
bool deleteOrTrashFile(const std::string& fullPath, bool& movedToTrash);
bool restoreTrashedFile(const std::string& fullPath, std::string& restoredPath);

}  // namespace BookActions
