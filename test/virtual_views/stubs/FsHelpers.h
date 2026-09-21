#pragma once
#include <strings.h>

#include <string_view>
namespace FsHelpers {
inline bool extension(std::string_view path, std::string_view suffix) {
  return path.size() >= suffix.size() &&
         strncasecmp(path.data() + path.size() - suffix.size(), suffix.data(), suffix.size()) == 0;
}
inline bool hasEpubExtension(std::string_view p) { return extension(p, ".epub"); }
inline bool hasXtcExtension(std::string_view p) { return extension(p, ".xtc") || extension(p, ".xtch"); }
inline bool hasTxtExtension(std::string_view p) { return extension(p, ".txt"); }
inline bool hasMarkdownExtension(std::string_view p) { return extension(p, ".md"); }
}  // namespace FsHelpers
