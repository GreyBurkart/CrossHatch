#include "VirtualViews.h"

#include <Arduino.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <TrashPaths.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "RecentBooksStore.h"
#include "activities/reader/BookReadingStats.h"

namespace VirtualViews {
namespace {
bool leapYear(const unsigned year) { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }

template <typename File>
auto iterationFailed(File& file, int) -> decltype(file.iterationFailed()) {
  return file.iterationFailed();
}
template <typename File>
bool iterationFailed(File&, long) {
  return false;
}

struct ScanWorkspace {
  explicit ScanWorkspace(const size_t limit) : candidates(limit) {}
  Candidates candidates;
  std::array<HalFile, MAX_FOLDER_DEPTH + 1> directories;
  std::array<size_t, MAX_FOLDER_DEPTH + 1> directoryLengths{};
  char path[PATH_CAPACITY] = "/";
  char name[PATH_CAPACITY]{};
};

float cachedEpubPercent(const std::string& cachePath) {
  HalFile file = Storage.open((cachePath + "/progress_percent.bin").c_str());
  if (!file) return -1.0f;
  uint8_t bytes[7]{};
  const bool valid = file.size() == sizeof(bytes) && file.read(bytes, sizeof(bytes)) == sizeof(bytes);
  file.close();
  // Existing EPRP v1 cache; do not rebuild missing caches or open a reader.
  if (!valid || bytes[0] != 0x50 || bytes[1] != 0x52 || bytes[2] != 0x50 || bytes[3] != 0x45 || bytes[4] != 1) {
    return -1.0f;
  }
  const unsigned basisPoints = bytes[5] | (static_cast<unsigned>(bytes[6]) << 8);
  return basisPoints <= 10000 ? static_cast<float>(basisPoints) / 100.0f : -1.0f;
}

bool finishedCandidate(const char* path, uint32_t& timestamp) {
  const std::string_view name(path);
  const bool epub = FsHelpers::hasEpubExtension(name);
  const bool xtc = FsHelpers::hasXtcExtension(name);
  if (!epub && !xtc) return false;
  const std::string bookPath(path);
  const std::string cache = epub ? Epub::cachePathForFilePath(bookPath, "/.crosspoint")
                                 : "/.crosspoint/xtc_" + std::to_string(std::hash<std::string>{}(bookPath));
  if (!Storage.exists(cache.c_str())) return false;
  const BookReadingStats stats = BookReadingStats::load(cache);
  if (!isFinished(stats.isCompleted, epub ? cachedEpubPercent(cache) : -1.0f)) return false;
  // A missing completion date sorts last; the book's modification date cannot
  // tell us when the user finished reading it.
  timestamp = stats.finishedDate.isValid() ? static_cast<uint32_t>(stats.finishedDate.year) * 10000 +
                                                 stats.finishedDate.month * 100 + stats.finishedDate.day
                                           : 0;
  return true;
}

ScanStatus collect(std::vector<RecentBook>& books, const size_t limit, const bool finished) {
  books.clear();
  ScanStatus status;
  if (limit == 0) return status;
  // About 11 KB, allocated once for this foreground scan: too large for the
  // activity stack, and keeping it static would permanently consume C3 RAM.
  auto work = makeUniqueNoThrow<ScanWorkspace>(std::min(limit, MAX_BOOKS));
  if (!work) {
    LOG_ERR("VVIEW", "Could not allocate scan workspace (%u bytes)", static_cast<unsigned>(sizeof(ScanWorkspace)));
    status.failed = true;
    return status;
  }
  work->directories[0] = Storage.open("/");
  if (!work->directories[0] || !work->directories[0].isDirectory()) {
    LOG_ERR("VVIEW", "Could not open library root");
    work->directories[0].close();
    status.failed = true;
    return status;
  }
  work->directoryLengths[0] = 1;
  size_t depth = 0;
  const unsigned long started = millis();
  while (true) {
    if (status.visited >= MAX_SCAN_ENTRIES || millis() - started >= MAX_SCAN_MS) {
      status.limited = true;
      break;
    }
    auto& directory = work->directories[depth];
    HalFile entry = directory.openNextFile();
    if (!entry) {
      if (entry.allocationFailed() || directory.allocationFailed() || iterationFailed(directory, 0)) {
        LOG_ERR("VVIEW", "Directory scan failed");
        status.failed = true;
      }
      entry.close();
      directory.close();
      if (depth == 0) break;
      --depth;
      continue;
    }
    ++status.visited;
    const size_t length = entry.getName(work->name, sizeof(work->name));
    const size_t parentLength = work->directoryLengths[depth];
    work->path[parentLength] = '\0';
    if (length == 0 || length >= sizeof(work->name) - 1) {
      status.limited = true;
      entry.close();
      continue;
    }
    const int appended = snprintf(work->path + parentLength, sizeof(work->path) - parentLength, "%s%s",
                                  parentLength > 1 ? "/" : "", work->name);
    if (appended < 0 || static_cast<size_t>(appended) >= sizeof(work->path) - parentLength) {
      status.limited = true;
      entry.close();
      continue;
    }
    if (!isVisiblePath(work->path)) {
      entry.close();
      continue;
    }
    if (entry.isDirectory()) {
      if (depth < MAX_FOLDER_DEPTH) {
        ++depth;
        work->directoryLengths[depth] = strlen(work->path);
        work->directories[depth] = std::move(entry);
      } else {
        entry.close();
      }
      continue;
    }
    const std::string_view name(work->name);
    const bool supported = FsHelpers::hasEpubExtension(name) || FsHelpers::hasXtcExtension(name) ||
                           FsHelpers::hasTxtExtension(name) || FsHelpers::hasMarkdownExtension(name);
    uint16_t date = 0;
    uint16_t time = 0;
    uint32_t timestamp =
        !finished && supported && readModifyDateTime(entry, &date, &time, 0) ? fatTimestamp(date, time) : 0;
    entry.close();  // Never overlap a book handle with its metadata/progress reads.
    if (!supported || (finished && !finishedCandidate(work->path, timestamp))) continue;
    status.unknownDates |= timestamp == 0;
    work->candidates.add(work->path, timestamp);
  }
  for (auto& directory : work->directories) directory.close();

  // Only the final (at most 18) UI records own strings. Reuse recents metadata
  // when available; other books use their filename without loading parsers.
  books.reserve(work->candidates.size());
  const auto& recents = RECENT_BOOKS.getBooks();
  for (size_t i = 0; i < work->candidates.size(); ++i) {
    const char* path = work->candidates[i].path;
    const auto recent =
        std::find_if(recents.begin(), recents.end(), [path](const RecentBook& book) { return book.path == path; });
    if (recent != recents.end()) {
      books.push_back(*recent);
    } else {
      books.push_back({path, strrchr(path, '/') + 1, "", ""});
    }
  }
  return status;
}
}  // namespace

uint32_t fatTimestamp(const uint16_t date, const uint16_t time) {
  const unsigned year = 1980 + (date >> 9);
  const unsigned month = (date >> 5) & 15;
  const unsigned day = date & 31;
  const unsigned hour = time >> 11;
  const unsigned minute = (time >> 5) & 63;
  const unsigned seconds = (time & 31) * 2;
  static constexpr uint8_t monthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12 || day < 1 || hour > 23 || minute > 59 || seconds > 59) return 0;
  if (day > monthDays[month - 1] + (month == 2 && leapYear(year) ? 1 : 0)) return 0;
  return (static_cast<uint32_t>(date) << 16) | time;
}

bool isFinished(const bool completed, const float progressPercent) {
  return completed || (std::isfinite(progressPercent) && progressPercent >= 99.5f && progressPercent <= 100.0f);
}

bool isVisiblePath(const char* path) {
  if (!path || path[0] != '/' || crosshatch::trash::isPath(path)) return false;
  for (const char* part = path; *part; ++part) {
    if (*part == '/' && part[1] == '.') return false;
  }
  return true;
}

Candidates::Candidates(const size_t limit) : limit_(std::min(limit, MAX_BOOKS)) {}

bool Candidates::add(const char* path, const uint32_t timestamp) {
  if (!path || strlen(path) >= PATH_CAPACITY || limit_ == 0) return false;
  for (size_t i = 0; i < size_; ++i) {
    if (strcmp(entries_[i].path, path) == 0) return false;
  }
  size_t position = 0;
  while (position < size_ &&
         (entries_[position].timestamp > timestamp ||
          (entries_[position].timestamp == timestamp && strcmp(entries_[position].path, path) < 0))) {
    ++position;
  }
  if (position >= limit_) return false;
  if (size_ < limit_) ++size_;
  for (size_t i = size_ - 1; i > position; --i) entries_[i] = entries_[i - 1];
  strcpy(entries_[position].path, path);
  entries_[position].timestamp = timestamp;
  return true;
}

ScanStatus loadRecentlyAdded(std::vector<RecentBook>& books, const size_t maxCount) {
  return collect(books, maxCount, false);
}

ScanStatus loadRecentlyFinished(std::vector<RecentBook>& books, const size_t maxCount) {
  return collect(books, maxCount, true);
}
}  // namespace VirtualViews
