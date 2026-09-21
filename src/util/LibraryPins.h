#pragma once

#include <cstddef>
#include <string>
#include <string_view>

class CrossPointSettings;

namespace LibraryPins {

inline constexpr size_t MAX_PATH_LENGTH = 1023;

// Exact absolute SD path, with no NUL, traversal, or empty components.
// Root is valid; other trailing slashes are rejected rather than normalized.
bool validPath(std::string_view path);

// Caller verifies that a newly pinned target exists and has the requested type.
// A stale unpin request leaves a replacement pin intact. Failed persistence
// restores both the previous path and Quick Action slots.
bool set(CrossPointSettings& settings, const std::string& path, bool directory, bool pin);

}  // namespace LibraryPins
