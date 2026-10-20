#include "Ao3UpdateParser.h"

#include <algorithm>
#include <cctype>

namespace {
constexpr size_t MAX_WINDOW_BYTES = 4096;
constexpr size_t RETAINED_WINDOW_BYTES = 2048;

std::string trim(std::string value) {
  const auto first = std::find_if_not(value.begin(), value.end(), [](const unsigned char c) { return std::isspace(c); });
  const auto last = std::find_if_not(value.rbegin(), value.rend(), [](const unsigned char c) { return std::isspace(c); });
  if (first == value.end()) return {};
  return std::string(first, last.base());
}

bool extractDefinition(const std::string& html, const char* className, std::string& value) {
  const auto hasClassToken = [className](const std::string& tag) {
    size_t classPos = tag.find("class");
    while (classPos != std::string::npos) {
      size_t cursor = classPos + 5;
      while (cursor < tag.size() && std::isspace(static_cast<unsigned char>(tag[cursor]))) ++cursor;
      if (cursor >= tag.size() || tag[cursor] != '=') {
        classPos = tag.find("class", classPos + 5);
        continue;
      }
      ++cursor;
      while (cursor < tag.size() && std::isspace(static_cast<unsigned char>(tag[cursor]))) ++cursor;
      if (cursor >= tag.size() || (tag[cursor] != '\"' && tag[cursor] != '\'')) return false;
      const char quote = tag[cursor++];
      const size_t end = tag.find(quote, cursor);
      if (end == std::string::npos) return false;
      const std::string classes = tag.substr(cursor, end - cursor);
      size_t tokenStart = 0;
      while (tokenStart < classes.size()) {
        while (tokenStart < classes.size() && std::isspace(static_cast<unsigned char>(classes[tokenStart]))) {
          ++tokenStart;
        }
        size_t tokenEnd = tokenStart;
        while (tokenEnd < classes.size() && !std::isspace(static_cast<unsigned char>(classes[tokenEnd]))) {
          ++tokenEnd;
        }
        if (classes.compare(tokenStart, tokenEnd - tokenStart, className) == 0) return true;
        tokenStart = tokenEnd;
      }
      return false;
    }
    return false;
  };

  size_t start = html.find("<dd");
  while (start != std::string::npos) {
    const size_t tagEnd = html.find('>', start + 3);
    if (tagEnd == std::string::npos) return false;
    if (hasClassToken(html.substr(start, tagEnd - start + 1))) {
      const size_t valueStart = tagEnd + 1;
      const size_t end = html.find("</dd>", valueStart);
      if (end == std::string::npos) return false;
      value = trim(html.substr(valueStart, end - valueStart));
      return !value.empty();
    }
    start = html.find("<dd", tagEnd + 1);
  }
  return false;
}

bool looksLikeIsoDate(const std::string& value) {
  if (value.size() < 10) return false;
  for (size_t i = 0; i < 10; ++i) {
    if (i == 4 || i == 7) {
      if (value[i] != '-') return false;
    } else if (!std::isdigit(static_cast<unsigned char>(value[i]))) {
      return false;
    }
  }
  return true;
}
}  // namespace

bool Ao3UpdateParser::feed(const uint8_t* data, const size_t length) {
  if (!data || length == 0 || complete()) return !complete();
  window_.append(reinterpret_cast<const char*>(data), length);
  parseWindow();
  if (window_.size() > MAX_WINDOW_BYTES) {
    window_.erase(0, window_.size() - RETAINED_WINDOW_BYTES);
  }
  return !complete();
}

void Ao3UpdateParser::parseWindow() {
  if (!result_.hasDate) {
    std::string date;
    if ((extractDefinition(window_, "status", date) || extractDefinition(window_, "published", date)) &&
        looksLikeIsoDate(date)) {
      result_.updatedDate = date.substr(0, 10);
      result_.hasDate = true;
    }
  }

  if (!result_.hasChapterStatus) {
    std::string chapters;
    if (extractDefinition(window_, "chapters", chapters)) {
      const size_t slash = chapters.find('/');
      if (slash != std::string::npos) {
        const std::string current = trim(chapters.substr(0, slash));
        const std::string total = trim(chapters.substr(slash + 1));
        result_.isCompleted = !current.empty() && total != "?" && current == total;
        result_.hasChapterStatus = true;
      }
    }
  }
}
