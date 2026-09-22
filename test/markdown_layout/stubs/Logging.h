#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace fixture {
inline std::vector<std::string> errors;
template <typename... Args>
inline void log(const char*, const char* format, Args... args) {
  if constexpr (sizeof...(args) == 0) {
    errors.emplace_back(format);
    return;
  }
  char text[256];
  if constexpr (sizeof...(args) > 0) std::snprintf(text, sizeof(text), format, args...);
  errors.emplace_back(text);
}
}  // namespace fixture
#define LOG_ERR(...) fixture::log(__VA_ARGS__)
