#pragma once
template <typename... Args>
inline void checklistTestLog(const char*, const char*, Args...) {}
#define LOG_ERR(...) checklistTestLog(__VA_ARGS__)
