#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// A bounded task-list view, not a Markdown rendering engine. Source bytes stay
// on SD; only headings, task labels and checkbox offsets remain in memory.
class MarkdownChecklist {
 public:
  static constexpr size_t MAX_FILE_BYTES = 64 * 1024;
  static constexpr size_t MAX_ROWS = 128;
  static constexpr size_t TEXT_BYTES = 8192;
  static constexpr size_t LINE_BYTES = 256;
  enum class Status { Ok, NoTasks, ReadError, LimitExceeded, NoMemory, SaveError, SourceChanged, RecoveryNeeded };
  struct Row {
    uint32_t markerOffset = 0;
    uint16_t textOffset = 0;
    bool heading = false;
    char marker = ' ';
    bool checked() const { return marker != ' '; }
  };

  explicit MarkdownChecklist(std::string path);
  Status load();
  Status toggle(size_t row);
  Status reset();
  size_t rowCount() const { return rowCount_; }
  size_t taskCount() const { return taskCount_; }
  size_t completedCount() const;
  const Row& row(size_t index) const { return data_->rows[index]; }
  const char* label(size_t index) const { return data_->text + row(index).textOffset; }
  const std::string& path() const { return path_; }

 private:
  struct Data {
    Row rows[MAX_ROWS];
    char text[TEXT_BYTES];
    char line[LINE_BYTES];
  };
  std::string path_;
  std::unique_ptr<Data> data_;
  size_t rowCount_ = 0;
  size_t taskCount_ = 0;
  size_t textUsed_ = 0;
  size_t fileSize_ = 0;
  uint64_t hash_ = 0;
  char fence_ = 0;
  size_t fenceLength_ = 0;
  bool frontMatter_ = false;
  bool comment_ = false;
  bool loaded_ = false;

  Status parseLine(size_t length, uint32_t offset, bool overflow);
  Status save(int targetRow);
  bool recover();
};
