#pragma once
#include <string>
#include <vector>
struct RecentBook {
  std::string path, title, author, coverBmpPath;
};
class RecentBooksStore {
 public:
  std::vector<RecentBook> books;
  const std::vector<RecentBook>& getBooks() const { return books; }
};
inline RecentBooksStore RECENT_BOOKS;
