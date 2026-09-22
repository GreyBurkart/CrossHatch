#pragma once

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace fixture {
inline std::map<std::string, std::string> files;
inline std::map<std::string, unsigned> handles;
inline std::set<std::string> failOpen;
inline unsigned overlapping = 0;
inline bool failRead = false;
inline bool failWrite = false;
inline bool failSync = false;
inline bool failSeek = false;
inline void reset() {
  files.clear();
  handles.clear();
  failOpen.clear();
  overlapping = 0;
  failRead = failWrite = failSync = failSeek = false;
}
}  // namespace fixture

class HalFile {
 public:
  HalFile() = default;
  ~HalFile() { close(); }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  bool attach(const std::string& path, bool write) {
    close();
    if (fixture::failOpen.count(path) || (!write && !fixture::files.count(path))) return false;
    path_ = path;
    writable_ = write;
    if (write) fixture::files[path].clear();
    if (++fixture::handles[path] > 1) ++fixture::overlapping;
    return true;
  }
  explicit operator bool() const { return !path_.empty(); }
  size_t size() const { return *this ? fixture::files.at(path_).size() : 0; }
  size_t position() const { return position_; }
  bool seek(size_t position) {
    if (!*this || fixture::failSeek || position > size()) return false;
    position_ = position;
    return true;
  }
  int read() {
    unsigned char c;
    return read(&c, 1) == 1 ? c : -1;
  }
  int read(void* target, size_t count) {
    if (!*this || fixture::failRead || position_ > size()) return -1;
    const auto& source = fixture::files.at(path_);
    count = std::min(count, source.size() - position_);
    std::memcpy(target, source.data() + position_, count);
    position_ += count;
    return static_cast<int>(count);
  }
  size_t write(const void* source, size_t count) {
    if (!*this || !writable_ || fixture::failWrite) return 0;
    auto& target = fixture::files.at(path_);
    target.resize(std::max(target.size(), position_ + count));
    std::memcpy(target.data() + position_, source, count);
    position_ += count;
    return count;
  }
  bool sync() const { return !fixture::failSync; }
  bool close() {
    if (*this) --fixture::handles[path_];
    path_.clear();
    position_ = 0;
    return true;
  }

 private:
  std::string path_;
  size_t position_ = 0;
  bool writable_ = false;
};

class HalStorage {
 public:
  bool openFileForRead(const char*, const std::string& path, HalFile& file) { return file.attach(path, false); }
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) { return file.attach(path, true); }
  bool remove(const char* path) {
    if (fixture::handles[path]) return false;
    return fixture::files.erase(path) != 0;
  }
};
inline HalStorage Storage;
inline void delay(unsigned) {}
