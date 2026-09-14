#pragma once

#include <string>
#include <string_view>

namespace ReadFolderPolicy {

inline constexpr std::string_view DEFAULT_FOLDER = "/Read";

inline std::string normalizeFolder(std::string folder) {
  if (folder.empty()) return std::string(DEFAULT_FOLDER);

  for (char& ch : folder) {
    if (ch == '\\') ch = '/';
  }
  if (folder.front() != '/') folder.insert(folder.begin(), '/');

  size_t write = 1;
  for (size_t read = 1; read < folder.size(); ++read) {
    if (folder[read] == '/' && folder[write - 1] == '/') continue;
    folder[write++] = folder[read];
  }
  folder.resize(write);
  while (folder.size() > 1 && folder.back() == '/') folder.pop_back();

  // Moving completed books to the SD root would make every folder a Read
  // descendant and suppress every recommendation. Keep the root selection safe.
  return folder == "/" ? std::string(DEFAULT_FOLDER) : folder;
}

inline bool isPathInNormalizedFolder(const std::string_view path, std::string_view folder) {
  if (folder.empty() || folder == "/") folder = DEFAULT_FOLDER;
  while (folder.size() > 1 && folder.back() == '/') folder.remove_suffix(1);
  return path == folder ||
         (path.size() > folder.size() && path.compare(0, folder.size(), folder) == 0 && path[folder.size()] == '/');
}

inline bool isPathInFolder(const std::string_view path, const std::string_view configuredFolder) {
  return isPathInNormalizedFolder(path, normalizeFolder(std::string(configuredFolder)));
}

}  // namespace ReadFolderPolicy
