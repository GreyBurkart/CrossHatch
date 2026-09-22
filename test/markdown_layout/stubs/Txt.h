#pragma once

#include <HalStorage.h>

#include <string>
#include <utility>

class Txt {
 public:
  explicit Txt(std::string path) : path_(std::move(path)) {}
  const std::string& getPath() const { return path_; }
  std::string getCachePath() const { return "/cache"; }
  size_t getFileSize() const { return fixture::files.at(path_).size(); }

 private:
  std::string path_;
};
