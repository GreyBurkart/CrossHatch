#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

struct RecentBook;

namespace VirtualViews {
enum class ViewMode : uint8_t { RecentlyOpened = 0, RecentlyAdded, RecentlyFinished };
constexpr size_t MAX_BOOKS = 18;
constexpr size_t MAX_FOLDER_DEPTH = 2;
constexpr size_t MAX_SCAN_ENTRIES = 2048;
constexpr unsigned long MAX_SCAN_MS = 2000;
constexpr size_t PATH_CAPACITY = 512;

struct ScanStatus {
  size_t visited = 0;
  bool limited = false;
  bool failed = false;
  bool unknownDates = false;
};

// Foreground, read-only scans of root and two folder levels. Added uses file
// modification dates, which are not necessarily the date copied onto the card.
ScanStatus loadRecentlyAdded(std::vector<RecentBook>& books, size_t maxCount = MAX_BOOKS);
ScanStatus loadRecentlyFinished(std::vector<RecentBook>& books, size_t maxCount = MAX_BOOKS);

// Small, platform-independent policy helpers shared by scanning and native tests.
uint32_t fatTimestamp(uint16_t date, uint16_t time);
bool isFinished(bool completed, float progressPercent);
bool isVisiblePath(const char* path);

struct Candidate {
  char path[PATH_CAPACITY]{};
  uint32_t timestamp = 0;
};

class Candidates {
 public:
  explicit Candidates(size_t limit = MAX_BOOKS);
  bool add(const char* path, uint32_t timestamp);
  size_t size() const { return size_; }
  const Candidate& operator[](size_t index) const { return entries_[index]; }

 private:
  std::array<Candidate, MAX_BOOKS> entries_{};
  size_t size_ = 0;
  size_t limit_;
};

// Older simulator HALs cannot provide these fields; never invent timestamps or
// mistake lack of timestamp support for a file-open failure.
template <typename File>
auto readModifyDateTime(File& file, uint16_t* date, uint16_t* time, int)
    -> decltype(file.getModifyDateTime(date, time)) {
  return file.getModifyDateTime(date, time);
}
template <typename File>
bool readModifyDateTime(File&, uint16_t*, uint16_t*, long) {
  return false;
}
}  // namespace VirtualViews
