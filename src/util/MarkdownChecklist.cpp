#include "MarkdownChecklist.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <string_view>
#include <utility>

namespace {
constexpr uint64_t HASH_START = 14695981039346656037ULL;
uint64_t hashByte(uint64_t hash, uint8_t byte) { return (hash ^ byte) * 1099511628211ULL; }
bool space(char ch) { return ch == ' ' || ch == '\t' || ch == '\r'; }
using Status = MarkdownChecklist::Status;
Status fail(Status status, const char* message) {
  LOG_ERR("CHECKLIST", "%s", message);
  return status;
}
}  // namespace

MarkdownChecklist::MarkdownChecklist(std::string path) : path_(std::move(path)) {}

bool MarkdownChecklist::recover() {
  const std::string backup = path_ + ".checklist.bak";
  const std::string temporary = path_ + ".checklist.tmp";
  if (Storage.exists(backup.c_str())) {
    // A cut between the two renames leaves the original in the backup. If the
    // new source exists, the synced replacement has already been installed.
    if (!Storage.exists(path_.c_str())) {
      if (!Storage.rename(backup.c_str(), path_.c_str())) return false;
    } else if (!Storage.remove(backup.c_str())) {
      return false;
    }
  }
  return !Storage.exists(temporary.c_str()) || Storage.remove(temporary.c_str());
}

MarkdownChecklist::Status MarkdownChecklist::load() {
  loaded_ = false;
  rowCount_ = taskCount_ = textUsed_ = 0;
  fence_ = 0;
  fenceLength_ = 0;
  frontMatter_ = comment_ = false;
  if (!recover()) return fail(Status::RecoveryNeeded, "Could not recover checklist save");
  auto source = Storage.open(path_.c_str());
  if (!source || source.isDirectory()) {
    source.close();
    return fail(Status::ReadError, "Could not open checklist");
  }
  fileSize_ = source.size();
  if (fileSize_ > MAX_FILE_BYTES) {
    source.close();
    return fail(Status::LimitExceeded, "Checklist exceeds 64 KiB");
  }
  // About 10 KiB, allocated once on entry and reused for parsing. Too large
  // for a task stack; static storage would retain it after closing the viewer.
  if (!data_) data_ = makeUniqueNoThrow<Data>();
  if (!data_) {
    source.close();
    return fail(Status::NoMemory, "Could not allocate checklist rows");
  }
  uint8_t buffer[256];
  size_t position = 0;
  size_t lineStart = 0;
  size_t lineLength = 0;
  bool overflow = false;
  hash_ = HASH_START;
  Status result = Status::Ok;
  while (position < fileSize_ && result == Status::Ok) {
    const size_t amount = std::min(sizeof(buffer), fileSize_ - position);
    if (source.read(buffer, amount) != static_cast<int>(amount)) {
      result = Status::ReadError;
      break;
    }
    for (size_t i = 0; i < amount; ++i, ++position) {
      const char ch = static_cast<char>(buffer[i]);
      hash_ = hashByte(hash_, buffer[i]);
      if (ch == '\0') {
        result = Status::ReadError;
        break;
      }
      if (ch == '\n') {
        result = parseLine(lineLength, lineStart, overflow);
        lineLength = 0;
        overflow = false;
        lineStart = position + 1;
        if (result != Status::Ok) break;
      } else if (lineLength + 1 < LINE_BYTES) {
        data_->line[lineLength++] = ch;
      } else {
        overflow = true;
      }
    }
  }
  if (result == Status::Ok && lineLength) result = parseLine(lineLength, lineStart, overflow);
  if (!source.close() && result == Status::Ok) result = Status::ReadError;
  if (result != Status::Ok) return fail(result, "Could not load complete checklist");
  loaded_ = true;
  return taskCount_ ? Status::Ok : Status::NoTasks;
}

MarkdownChecklist::Status MarkdownChecklist::parseLine(size_t length, uint32_t offset, bool overflow) {
  data_->line[length] = '\0';
  std::string_view line(data_->line, length);
  size_t start = 0;
  if (offset == 0 && line.substr(0, 3) == "\xEF\xBB\xBF") start = 3;
  while (start < line.size() && space(line[start])) ++start;
  size_t end = line.size();
  while (end > start && space(line[end - 1])) --end;
  const auto trimmed = line.substr(start, end - start);
  if (offset == 0 && trimmed == "---") {
    frontMatter_ = true;
    return Status::Ok;
  }
  if (frontMatter_) {
    if (trimmed == "---" || trimmed == "...") frontMatter_ = false;
    return Status::Ok;
  }
  if (fence_) {
    size_t count = 0;
    while (count < trimmed.size() && trimmed[count] == fence_) ++count;
    if (count >= fenceLength_ && count == trimmed.size()) fence_ = 0;
    return Status::Ok;
  }
  if (comment_ || trimmed.find("<!--") != std::string_view::npos) {
    comment_ = trimmed.find("-->") == std::string_view::npos;
    return Status::Ok;
  }
  if (trimmed.size() >= 3 && (trimmed[0] == '`' || trimmed[0] == '~')) {
    size_t count = 0;
    while (count < trimmed.size() && trimmed[count] == trimmed[0]) ++count;
    if (count >= 3) {
      fence_ = trimmed[0];
      fenceLength_ = count;
      return Status::Ok;
    }
  }
  if (trimmed.empty()) return Status::Ok;
  size_t textStart = start;
  size_t marker = 0;
  bool heading = false;
  if (line[start] == '#') {
    while (textStart < end && line[textStart] == '#') ++textStart;
    heading = textStart - start <= 6 && textStart < end && space(line[textStart]);
    if (!heading) return Status::Ok;
    // Remove an optional closing ATX heading marker.
    size_t closing = end;
    while (closing > textStart && line[closing - 1] == '#') --closing;
    if (closing < end && closing > textStart && space(line[closing - 1])) end = closing - 1;
  } else {
    if (line[textStart] == '-' || line[textStart] == '*' || line[textStart] == '+') {
      ++textStart;
    } else {
      // Common Markdown ordered task lists, including nested items.
      while (textStart < end && line[textStart] >= '0' && line[textStart] <= '9') ++textStart;
      if (textStart == start || textStart - start > 9 || textStart == end ||
          (line[textStart] != '.' && line[textStart] != ')'))
        return Status::Ok;
      ++textStart;
    }
    if (textStart == end || !space(line[textStart])) return Status::Ok;
    while (textStart < end && space(line[textStart])) ++textStart;
    if (textStart + 3 > end || line[textStart] != '[' || line[textStart + 2] != ']' ||
        (line[textStart + 1] != ' ' && line[textStart + 1] != 'x' && line[textStart + 1] != 'X') ||
        (textStart + 3 < end && !space(line[textStart + 3])))
      return Status::Ok;
    marker = textStart + 1;
    textStart += 3;
  }
  while (textStart < end && space(line[textStart])) ++textStart;
  while (end > textStart && space(line[end - 1])) --end;
  const size_t textLength = end - textStart;
  if (overflow || rowCount_ == MAX_ROWS || textUsed_ + textLength + 1 > TEXT_BYTES) return Status::LimitExceeded;
  auto& row = data_->rows[rowCount_++];
  row.heading = heading;
  row.textOffset = textUsed_;
  row.markerOffset = offset + marker;
  row.marker = heading ? ' ' : line[marker];
  memcpy(data_->text + textUsed_, line.data() + textStart, textLength);
  data_->text[textUsed_ + textLength] = '\0';
  textUsed_ += textLength + 1;
  if (!heading) ++taskCount_;
  return Status::Ok;
}

size_t MarkdownChecklist::completedCount() const {
  size_t completed = 0;
  for (size_t i = 0; i < rowCount_; ++i) {
    if (!row(i).heading && row(i).checked()) ++completed;
  }
  return completed;
}

MarkdownChecklist::Status MarkdownChecklist::toggle(size_t index) {
  if (!loaded_ || index >= rowCount_ || row(index).heading) return fail(Status::SaveError, "Invalid checklist row");
  return save(static_cast<int>(index));
}

MarkdownChecklist::Status MarkdownChecklist::reset() {
  if (!loaded_) return fail(Status::SaveError, "Checklist is not loaded");
  return completedCount() ? save(-1) : Status::Ok;
}

MarkdownChecklist::Status MarkdownChecklist::save(int targetRow) {
  if (!recover()) return fail(Status::RecoveryNeeded, "Could not recover prior save");
  const std::string temporary = path_ + ".checklist.tmp";
  const std::string backup = path_ + ".checklist.bak";
  auto source = Storage.open(path_.c_str());
  if (!source) return fail(Status::SaveError, "Could not reopen checklist");
  if (source.size() != fileSize_) {
    source.close();
    return fail(Status::SourceChanged, "Checklist changed since opening");
  }
  auto output = Storage.open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL);
  if (!output) {
    source.close();
    return fail(Status::SaveError, "Could not create checklist temporary file");
  }
  uint8_t buffer[256];
  uint64_t oldHash = HASH_START;
  uint64_t newHash = HASH_START;
  size_t nextRow = 0;
  Status result = Status::Ok;
  for (size_t offset = 0; offset < fileSize_ && result == Status::Ok;) {
    const size_t count = std::min(sizeof(buffer), fileSize_ - offset);
    if (source.read(buffer, count) != static_cast<int>(count)) {
      result = Status::SaveError;
      break;
    }
    for (size_t i = 0; i < count; ++i) oldHash = hashByte(oldHash, buffer[i]);
    while (nextRow < rowCount_) {
      const auto& item = row(nextRow);
      if (item.heading) {
        ++nextRow;
        continue;
      }
      if (item.markerOffset >= offset + count) break;
      if (targetRow < 0 || static_cast<size_t>(targetRow) == nextRow) {
        buffer[item.markerOffset - offset] = targetRow < 0 || item.checked() ? ' ' : 'x';
      }
      ++nextRow;
    }
    for (size_t i = 0; i < count; ++i) newHash = hashByte(newHash, buffer[i]);
    if (output.write(buffer, count) != count) result = Status::SaveError;
    offset += count;
  }
  if (result == Status::Ok && oldHash != hash_) result = Status::SourceChanged;
  if (!source.close() && result == Status::Ok) result = Status::SaveError;
  if (!output.sync() && result == Status::Ok) result = Status::SaveError;
  if (!output.close() && result == Status::Ok) result = Status::SaveError;
  if (result != Status::Ok) {
    if (!Storage.remove(temporary.c_str())) LOG_ERR("CHECKLIST", "Could not remove failed temporary save");
    return fail(result, "Checklist save failed; source kept");
  }
  // Both handles are closed before renaming; the synced replacement changes
  // only checkbox marker bytes, preserving line endings and all other Markdown.
  if (!Storage.rename(path_.c_str(), backup.c_str())) {
    if (!Storage.remove(temporary.c_str())) LOG_ERR("CHECKLIST", "Could not remove temporary save");
    return fail(Status::SaveError, "Could not back up checklist");
  }
  if (!Storage.rename(temporary.c_str(), path_.c_str())) {
    if (!Storage.rename(backup.c_str(), path_.c_str())) {
      loaded_ = false;
      return fail(Status::RecoveryNeeded, "Original checklist retained in .checklist.bak");
    }
    if (!Storage.remove(temporary.c_str())) LOG_ERR("CHECKLIST", "Could not remove temporary save");
    return fail(Status::SaveError, "Checklist replacement failed; original restored");
  }
  hash_ = newHash;
  for (size_t i = 0; i < rowCount_; ++i) {
    if (!row(i).heading && (targetRow < 0 || static_cast<size_t>(targetRow) == i)) {
      data_->rows[i].marker = targetRow < 0 || row(i).checked() ? ' ' : 'x';
    }
  }
  if (!Storage.remove(backup.c_str())) LOG_ERR("CHECKLIST", "Saved; backup cleanup deferred until next open");
  return Status::Ok;
}
