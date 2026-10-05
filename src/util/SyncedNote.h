#pragma once

#include <cstddef>
#include <string_view>

// One externally owned document; uploading it replaces local checklist edits.
namespace SyncedNote {
inline constexpr char DIRECTORY[] = "/CrossHatch/Notes";
inline constexpr char PATH[] = "/CrossHatch/Notes/TheList.md";
inline constexpr char TEMP_PATH[] = "/CrossHatch/Notes/TheList.md.upload.tmp";
inline constexpr char BACKUP_PATH[] = "/CrossHatch/Notes/TheList.md.upload.bak";
inline constexpr size_t MAX_BYTES = 64 * 1024;

inline bool isPath(std::string_view path) { return path == PATH; }
bool recover();
bool prepareUpload();
// The caller must sync and close the temporary file before publishing it.
bool finishUpload();
void discardUpload();
}  // namespace SyncedNote
