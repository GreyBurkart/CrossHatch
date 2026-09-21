#pragma once
#include <string>
struct Epub {
  static std::string cachePathForFilePath(const std::string& path, const std::string& root) {
    return root + "/epub_" + std::to_string(std::hash<std::string>{}(path));
  }
};
