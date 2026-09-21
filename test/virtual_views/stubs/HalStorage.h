#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace fixture {
struct File {
  bool directory = false;
  bool failedIteration = false;
  bool failedAllocation = false;
  uint16_t date = 0;
  uint16_t time = 0;
  std::vector<std::string> children;
  std::vector<uint8_t> data;
};
inline std::map<std::string, File> files;
inline std::map<std::string, unsigned> handles;
inline unsigned overlappingHandles = 0;
inline unsigned long clock = 0;
inline unsigned long tick = 0;
inline void reset() {
  files.clear();
  handles.clear();
  overlappingHandles = 0;
  clock = 0;
  tick = 0;
}
inline void add(const std::string& path, bool directory = false, uint16_t date = 0, uint16_t time = 0) {
  files[path] = {directory, false, false, date, time, {}, {}};
  if (path == "/") return;
  const auto slash = path.rfind('/');
  const std::string parent = slash == 0 ? "/" : path.substr(0, slash);
  files[parent].children.push_back(path);
}
}  // namespace fixture
class HalFile {
 public:
  HalFile() = default;
  explicit HalFile(std::string path) : path_(std::move(path)) {
    if (++fixture::handles[path_] > 1) ++fixture::overlappingHandles;
  }
  ~HalFile() { close(); }
  HalFile(HalFile&& other) noexcept { *this = std::move(other); }
  HalFile& operator=(HalFile&& other) noexcept {
    if (this != &other) {
      close();
      path_ = std::move(other.path_);
      other.path_.clear();
      next_ = other.next_;
    }
    return *this;
  }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  explicit operator bool() const { return !path_.empty(); }
  bool isDirectory() const { return *this && fixture::files.at(path_).directory; }
  bool close() {
    if (*this) --fixture::handles[path_];
    path_.clear();
    return true;
  }
  bool allocationFailed() const { return *this && fixture::files.at(path_).failedAllocation; }
  bool iterationFailed() const { return *this && fixture::files.at(path_).failedIteration; }
  HalFile openNextFile() {
    const auto& children = fixture::files.at(path_).children;
    return next_ < children.size() ? HalFile(children[next_++]) : HalFile{};
  }
  size_t getName(char* out, size_t capacity) {
    const auto name = path_.substr(path_.rfind('/') + 1);
    const size_t count = name.size() < capacity - 1 ? name.size() : capacity - 1;
    memcpy(out, name.data(), count);
    out[count] = '\0';
    return count;
  }
  bool getModifyDateTime(uint16_t* date, uint16_t* time) {
    const auto& file = fixture::files.at(path_);
    *date = file.date;
    *time = file.time;
    return file.date != 0;
  }
  size_t size() const { return fixture::files.at(path_).data.size(); }
  int read(void* data, size_t size) {
    const auto& source = fixture::files.at(path_).data;
    const size_t count = size < source.size() ? size : source.size();
    memcpy(data, source.data(), count);
    return static_cast<int>(count);
  }

 private:
  std::string path_;
  size_t next_ = 0;
};
class HalStorage {
 public:
  HalFile open(const char* path) { return exists(path) ? HalFile(path) : HalFile{}; }
  bool exists(const char* path) const { return fixture::files.count(path) != 0; }
};
inline HalStorage Storage;
