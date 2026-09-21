#pragma once

#include <fcntl.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace fixture {
inline std::map<std::string, std::string> files;
inline std::map<std::string, unsigned> handles;
inline unsigned overlapping = 0;
inline unsigned renameCount = 0;
inline std::set<unsigned> failRenames;
inline bool failWrite = false;
inline bool failSync = false;
inline bool failClose = false;
inline bool failRead = false;
inline bool failCreate = false;
inline bool failRemove = false;
inline void reset() {
  files.clear();
  handles.clear();
  overlapping = renameCount = 0;
  failRenames.clear();
  failWrite = failSync = failClose = failRead = failCreate = failRemove = false;
}
}  // namespace fixture

class HalFile {
 public:
  HalFile() = default;
  HalFile(std::string path, bool writable) : path_(std::move(path)), writable_(writable) {
    if (++fixture::handles[path_] > 1) ++fixture::overlapping;
  }
  ~HalFile() { close(); }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  HalFile(HalFile&& other) noexcept
      : path_(std::move(other.path_)), writable_(other.writable_), position_(other.position_) {
    other.path_.clear();
  }
  explicit operator bool() const { return !path_.empty(); }
  bool isDirectory() const { return false; }
  size_t size() const { return fixture::files.at(path_).size(); }
  int read(void* target, size_t count) {
    if (fixture::failRead) return -1;
    const auto& source = fixture::files.at(path_);
    count = std::min(count, source.size() - position_);
    memcpy(target, source.data() + position_, count);
    position_ += count;
    return static_cast<int>(count);
  }
  size_t write(const void* source, size_t count) {
    if (!writable_) return 0;
    if (fixture::failWrite && count) --count;
    fixture::files.at(path_).append(static_cast<const char*>(source), count);
    return count;
  }
  bool sync() const { return !fixture::failSync; }
  bool close() {
    if (*this) --fixture::handles[path_];
    path_.clear();
    return !fixture::failClose;
  }

 private:
  std::string path_;
  bool writable_ = false;
  size_t position_ = 0;
};

class HalStorage {
 public:
  HalFile open(const char* path, int flags = O_RDONLY) {
    const bool writable = (flags & O_WRONLY) != 0;
    if (flags & O_CREAT) {
      if (fixture::failCreate || ((flags & O_EXCL) && exists(path))) return {};
      fixture::files.try_emplace(path, "");
    }
    if (!exists(path)) return {};
    return HalFile(path, writable);
  }
  bool exists(const char* path) const { return fixture::files.count(path) != 0; }
  bool remove(const char* path) {
    if (fixture::failRemove || fixture::handles[path]) return false;
    return fixture::files.erase(path) != 0;
  }
  bool rename(const char* from, const char* to) {
    if (fixture::failRenames.count(++fixture::renameCount) || !exists(from) || exists(to) || fixture::handles[from] ||
        fixture::handles[to])
      return false;
    fixture::files.emplace(to, std::move(fixture::files.at(from)));
    fixture::files.erase(from);
    return true;
  }
};
inline HalStorage Storage;
