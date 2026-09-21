#pragma once
#include <cstdint>
#include <map>
#include <string>
struct ReadingStatsDate {
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  bool isValid() const { return year >= 1980 && month >= 1 && month <= 12 && day >= 1 && day <= 31; }
};
struct BookReadingStats {
  bool isCompleted = false;
  ReadingStatsDate finishedDate;
  static BookReadingStats load(const std::string& path);
};
namespace fixture {
inline std::map<std::string, BookReadingStats> stats;
}
inline BookReadingStats BookReadingStats::load(const std::string& path) {
  return fixture::stats.count(path) ? fixture::stats.at(path) : BookReadingStats{};
}
