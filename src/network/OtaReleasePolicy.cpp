#include "OtaReleasePolicy.h"

#include <cstddef>
#include <cstring>
#include <limits>

namespace ota_release_policy {
namespace {

constexpr size_t SEGMENT_COUNT = 4;

struct ParsedVersion {
  int core[SEGMENT_COUNT] = {0, 0, 0, 0};
  int ao3[SEGMENT_COUNT] = {0, 0, 0, 0};
  bool valid = false;
  bool hasAo3 = false;
  bool releaseCandidate = false;
};

bool isDigit(const char c) { return c >= '0' && c <= '9'; }

char asciiLower(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool startsWith(const char* value, const char* prefix) {
  if (value == nullptr || prefix == nullptr) return false;
  return std::strncmp(value, prefix, std::strlen(prefix)) == 0;
}

bool endsWith(const char* value, const char* suffix) {
  if (value == nullptr || suffix == nullptr) return false;
  const size_t valueLength = std::strlen(value);
  const size_t suffixLength = std::strlen(suffix);
  return suffixLength <= valueLength && std::strcmp(value + valueLength - suffixLength, suffix) == 0;
}

const char* findCaseInsensitive(const char* value, const char* needle) {
  if (value == nullptr || needle == nullptr || needle[0] == '\0') return nullptr;
  const size_t needleLength = std::strlen(needle);
  for (const char* p = value; *p != '\0'; ++p) {
    size_t i = 0;
    while (i < needleLength && p[i] != '\0' && asciiLower(p[i]) == asciiLower(needle[i])) ++i;
    if (i == needleLength) return p;
  }
  return nullptr;
}

bool containsRcMarker(const char* version) { return findCaseInsensitive(version, "-rc") != nullptr; }

bool parseSegments(const char* value, int (&segments)[SEGMENT_COUNT]) {
  if (value == nullptr || !isDigit(*value)) return false;

  const char* p = value;
  for (size_t index = 0; index < SEGMENT_COUNT; ++index) {
    if (!isDigit(*p)) return index > 0;

    int segment = 0;
    while (isDigit(*p)) {
      const int digit = *p - '0';
      if (segment > (std::numeric_limits<int>::max() - digit) / 10) return false;
      segment = segment * 10 + digit;
      ++p;
    }
    segments[index] = segment;

    if (*p != '.' || !isDigit(p[1])) return true;
    ++p;
  }
  return true;
}

ParsedVersion parseVersion(const char* version) {
  ParsedVersion parsed;
  if (version == nullptr) return parsed;

  const char* core = version;
  if ((*core == 'v' || *core == 'V') && isDigit(core[1])) ++core;
  if (!parseSegments(core, parsed.core)) return parsed;

  if (const char* ao3Marker = findCaseInsensitive(core, "-ao3.")) {
    parsed.hasAo3 = true;
    if (!parseSegments(ao3Marker + std::strlen("-ao3."), parsed.ao3)) return parsed;
  }

  parsed.releaseCandidate = containsRcMarker(core);
  parsed.valid = true;
  return parsed;
}

int compareSegments(const int (&left)[SEGMENT_COUNT], const int (&right)[SEGMENT_COUNT]) {
  for (size_t i = 0; i < SEGMENT_COUNT; ++i) {
    if (left[i] != right[i]) return left[i] > right[i] ? 1 : -1;
  }
  return 0;
}

bool endsWithDeviceSuffix(const char* assetName, const char* deviceType) {
  if (assetName == nullptr || deviceType == nullptr || deviceType[0] == '\0') return false;
  const size_t nameLength = std::strlen(assetName);
  const size_t deviceLength = std::strlen(deviceType);
  constexpr size_t punctuationLength = 1 + sizeof(".bin") - 1;
  const size_t suffixLength = deviceLength + punctuationLength;
  if (nameLength < suffixLength) return false;

  const char* suffix = assetName + nameLength - suffixLength;
  return suffix[0] == '-' && std::strncmp(suffix + 1, deviceType, deviceLength) == 0 &&
         std::strcmp(suffix + 1 + deviceLength, ".bin") == 0;
}

}  // namespace

int compareVersions(const char* latest, const char* current) {
  const ParsedVersion parsedLatest = parseVersion(latest);
  const ParsedVersion parsedCurrent = parseVersion(current);
  if (!parsedLatest.valid || !parsedCurrent.valid) return 0;

  if (const int coreComparison = compareSegments(parsedLatest.core, parsedCurrent.core); coreComparison != 0) {
    return coreComparison;
  }

  if (parsedLatest.hasAo3 != parsedCurrent.hasAo3) return parsedLatest.hasAo3 ? 1 : -1;
  if (parsedLatest.hasAo3) {
    if (const int ao3Comparison = compareSegments(parsedLatest.ao3, parsedCurrent.ao3); ao3Comparison != 0) {
      return ao3Comparison;
    }
  }

  if (parsedLatest.releaseCandidate != parsedCurrent.releaseCandidate) {
    return parsedLatest.releaseCandidate ? -1 : 1;
  }
  return 0;
}

bool matchesFirmwareAsset(const char* assetName, const char* deviceType) {
  if (assetName == nullptr || deviceType == nullptr || deviceType[0] == '\0') return false;

  constexpr char canonicalPrefix[] = "firmware-";
  if (startsWith(assetName, canonicalPrefix)) {
    const char* assetDevice = assetName + sizeof(canonicalPrefix) - 1;
    const size_t deviceLength = std::strlen(deviceType);
    if (std::strncmp(assetDevice, deviceType, deviceLength) == 0 &&
        (std::strcmp(assetDevice + deviceLength, ".bin") == 0 || assetDevice[deviceLength] == '-')) {
      return endsWith(assetName, ".bin");
    }
  }

  return startsWith(assetName, "CrossInk-AO3-") && endsWithDeviceSuffix(assetName, deviceType);
}

}  // namespace ota_release_policy
