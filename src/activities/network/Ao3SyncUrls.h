#pragma once

#include <string>

namespace Ao3SyncUrls {

inline std::string workPage(const char* host, const std::string& workId) {
  // view_full_work prevents AO3 from redirecting chaptered works to the latest
  // chapter. That redirect relies on a view_adult cookie which the low-memory
  // streaming client intentionally does not retain between requests.
  return std::string("https://") + host + "/works/" + workId + "?view_full_work=true&view_adult=true";
}

inline std::string download(const char* host, const std::string& workId, const std::string& date) {
  std::string url = std::string("https://") + host + "/downloads/" + workId + "/work.epub";
  if (!date.empty()) url += "?v=" + date;
  return url;
}

}  // namespace Ao3SyncUrls
