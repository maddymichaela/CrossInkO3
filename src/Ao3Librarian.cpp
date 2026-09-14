#include "Ao3Librarian.h"
#include "Ao3HtmlMetadataParser.h"
#include "Ao3ReadingState.h"
#include <memory>
#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ZipFile.h>
#include <functional>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace {
constexpr const char* AO3_CACHE_ROOT = "/.crosspoint";
constexpr const char* AO3_INDEX_PATH = "/.crosspoint/ao3_library_index.bin";
constexpr uint8_t INDEX_VERSION = 1;
constexpr size_t RECENT_INFO_PATH_COUNT = 12;
constexpr size_t INFO_LOCATOR_COUNT = MAX_LIBRARY_BOOKS;

struct RecentInfoPath {
  uint32_t cacheHash = 0;
  std::string infoPath;
};

std::vector<RecentInfoPath> recentInfoPaths;
std::vector<uint64_t> infoLocators;
Ao3LibrarySummary cachedLibrarySummary;
bool librarySummaryValid = false;

std::string infoPathForHash(const uint64_t fullHash) {
  return std::string(AO3_CACHE_ROOT) + "/epub_" + std::to_string(fullHash) + "/ao3_library_info";
}

void rememberInfoLocator(const uint64_t fullHash) {
  if (std::find(infoLocators.begin(), infoLocators.end(), fullHash) != infoLocators.end()) return;
  if (infoLocators.size() >= INFO_LOCATOR_COUNT) return;
  if (infoLocators.capacity() == 0) infoLocators.reserve(INFO_LOCATOR_COUNT);
  infoLocators.push_back(fullHash);
}

void rememberInfoPath(const uint32_t cacheHash, const std::string& infoPath) {
  recentInfoPaths.erase(
      std::remove_if(recentInfoPaths.begin(), recentInfoPaths.end(),
                     [cacheHash](const RecentInfoPath& item) { return item.cacheHash == cacheHash; }),
      recentInfoPaths.end());
  recentInfoPaths.insert(recentInfoPaths.begin(), RecentInfoPath{cacheHash, infoPath});
  if (recentInfoPaths.size() > RECENT_INFO_PATH_COUNT) recentInfoPaths.pop_back();
}

bool containsCaseInsensitive(const char* haystack, const char* needle) {
  if (!haystack || !needle || !*needle) {
    return false;
  }

  const size_t needleLen = strlen(needle);
  for (const char* p = haystack; *p; ++p) {
    if (strncasecmp(p, needle, needleLen) == 0) {
      return true;
    }
  }
  return false;
}

uint32_t ao3PathHash(const std::string& path) {
  return static_cast<uint32_t>(ZipFile::fnvHash64(path.c_str(), path.size()));
}

uint32_t ao3PathHash(const char* path) {
  return path ? static_cast<uint32_t>(ZipFile::fnvHash64(path, strlen(path))) : 0;
}

bool readIndexHeader(HalFile& file, uint16_t& recordCount, uint32_t* nextSequence = nullptr) {
  char magic[4];
  uint8_t version = 0;
  uint32_t sequence = 0;

  if (file.read(magic, 4) != 4 || file.read(&version, 1) != 1 ||
      file.read(reinterpret_cast<uint8_t*>(&recordCount), 2) != 2 ||
      file.read(reinterpret_cast<uint8_t*>(&sequence), 4) != 4) {
    return false;
  }

  uint8_t reserved = 0;
  if (file.read(&reserved, 1) != 1) {
    return false;
  }

  if (memcmp(magic, "AO3X", 4) != 0 || version != INDEX_VERSION || recordCount > MAX_LIBRARY_BOOKS) {
    return false;
  }

  if (nextSequence) {
    *nextSequence = sequence;
  }
  return true;
}

bool readAo3LibraryInfoAtPath(const std::string& infoPath, Ao3LibraryMetadata& meta) {
  HalFile file;
  if (!Storage.openFileForRead("AO3L", infoPath, file)) {
    return false;
  }

  const bool ok = file.read(reinterpret_cast<uint8_t*>(&meta), sizeof(meta)) == sizeof(meta);
  file.close();
  return ok && meta.isValid() && meta.version == 8;
}

bool cacheHashFromInfoPath(const std::string& infoPath, uint32_t& cacheHash, uint64_t* fullCacheHash = nullptr) {
  constexpr char marker[] = "/epub_";
  const size_t markerPos = infoPath.rfind(marker);
  if (markerPos == std::string::npos) return false;

  const char* number = infoPath.c_str() + markerPos + sizeof(marker) - 1;
  char* end = nullptr;
  const unsigned long long fullHash = strtoull(number, &end, 10);
  if (end == number || !end || *end != '/') return false;
  if (fullCacheHash) *fullCacheHash = static_cast<uint64_t>(fullHash);
  cacheHash = static_cast<uint32_t>(fullHash);
  return true;
}

template <typename Callback>
void forEachAo3InfoSidecar(Callback callback, const bool* stop = nullptr) {
  HalFile root = Storage.open(AO3_CACHE_ROOT);
  if (!root || !root.isDirectory()) {
    return;
  }

  for (HalFile entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    char name[64] = {};
    entry.getName(name, sizeof(name));
    const bool isEpubCacheDir = entry.isDirectory() && strncmp(name, "epub_", 5) == 0;
    entry.close();

    if (!isEpubCacheDir) {
      yield();
      continue;
    }

    const std::string infoPath = std::string(AO3_CACHE_ROOT) + "/" + name + "/ao3_library_info";
    if (Storage.exists(infoPath.c_str())) {
      uint32_t cacheHash = 0;
      uint64_t fullCacheHash = 0;
      if (cacheHashFromInfoPath(infoPath, cacheHash, &fullCacheHash)) rememberInfoLocator(fullCacheHash);
      callback(infoPath);
    }
    if (stop && *stop) break;
    yield();
  }

  root.close();
}

/**
 * @brief Internal stream consumer that parses AO3 metadata on the fly.
 * Uses fixed-size buffers to avoid heap fragmentation during heavy indexing.
 */
class HtmlScraper : public Print {
 public:
  Ao3LibraryMetadata& meta;
  char buffer[2048];
  size_t bufferSize = 0;
  bool inSummary = false;
  bool inTag = false;
  size_t summaryBytes = 0;
  bool isInProgress = false;
  std::string scrapedWorkId;
  std::string scrapedDate;
  bool hasUpdatedDate = false;
  char scrapedFandom[32];
  char scrapedRel1[32];
  char scrapedRel2[32];

  explicit HtmlScraper(Ao3LibraryMetadata& m) : meta(m), scrapedWorkId(""), scrapedDate(""), hasUpdatedDate(false) {
    memset(buffer, 0, sizeof(buffer));
    bufferSize = 0;
    summaryBytes = 0;
    inSummary = false;
    inTag = false;
    isInProgress = false;
    memset(scrapedFandom, 0, sizeof(scrapedFandom));
    memset(scrapedRel1, 0, sizeof(scrapedRel1));
    memset(scrapedRel2, 0, sizeof(scrapedRel2));
  }

  void extractCommaSeparatedFields(const char* anchor, char* const outputs[], int maxCount, bool isRelationship = false) {
    const char* pos = strstr(buffer, anchor);
    if (!pos) return;

    const char* scan = pos + strlen(anchor);

    // Skip to the start of the first value: HTML tags (</dt>, <dd ...>,
    // <a href="...">, </b>), whitespace, and leading punctuation/quotes
    // (":", the opening '"' in FFF's quoted string, etc).
    while (scan < buffer + bufferSize) {
        if (*scan == '<') {
            while (scan < buffer + bufferSize && *scan != '>') scan++;
            if (scan < buffer + bufferSize) scan++;
        } else if (isspace(static_cast<unsigned char>(*scan)) || *scan == ':' ||
                   *scan == '"' || *scan == '/' || *scan == '-' || *scan == '_') {
            scan++;
        } else break;
    }

    int filled = 0;
    while (filled < maxCount && scan < buffer + bufferSize) {
        const char* comma = strchr(scan, ',');
        const char* lt    = strchr(scan, '<');
        const char* quote = isRelationship ? nullptr : strchr(scan, '"');

        // Earliest of the three terminators that is non-null
        const char* end = nullptr;
        for (const char* cand : { comma, lt, quote }) {
            if (cand && (!end || cand < end)) end = cand;
        }
        if (!end) break;

        // Native AO3: scan sits directly on <a href="...">. Step into its
        // text content, then re-evaluate — </a> becomes the terminator.
        if (lt && lt == scan) {
            while (scan < buffer + bufferSize && *scan != '>') scan++;
            if (scan < buffer + bufferSize) scan++;
            lt = strchr(scan, '<');
            if (!lt) break;
            end = lt;
        }

        size_t rawLen = (size_t)(end - scan);
        if (rawLen == 0) {
            // Empty token (consecutive delimiters) — skip and retry
            scan = end + 1;
            continue;
        }

        if (isRelationship) {
            // If the number of quotes in the extracted token is odd,
            // the final quote is an unbalanced wrapping quote from FFF.
            size_t qCount = 0;
            for (size_t i = 0; i < rawLen; i++) {
                if (scan[i] == '"') qCount++;
            }
            if (qCount % 2 != 0 && rawLen > 0 && scan[rawLen - 1] == '"') {
                rawLen--;
            }
        }

        // &amp; cleanup
        size_t outIdx = 0;
        size_t srcIdx = 0;
        while (srcIdx < rawLen && outIdx < 31) {
            if (scan[srcIdx] == '&' && (rawLen - srcIdx) >= 5 &&
                strncasecmp(&scan[srcIdx], "&amp;", 5) == 0) {
                outputs[filled][outIdx++] = '&';
                srcIdx += 5;
            } else {
                outputs[filled][outIdx++] = scan[srcIdx++];
            }
        }
        outputs[filled][outIdx] = '\0';
        size_t len = outIdx;

        // Trim trailing whitespace
        for (int i = (int)len - 1;
             i >= 0 && isspace(static_cast<unsigned char>(outputs[filled][i])); i--)
            outputs[filled][i] = '\0';
        //tag spillover prevention
        if (strcmp(outputs[filled], "Status:") == 0 ||
            strcmp(outputs[filled], "Characters:") == 0 ) {
            outputs[filled][0] = '\0'; // Clear out the bogus metadata label
            break;                     // Stop parsing further slots
        }
        filled++;

        if (end == lt) {
            // Native AO3: skip past </a>
            scan = lt;
            while (scan < buffer + bufferSize && *scan != '>') scan++;
            if (scan < buffer + bufferSize) scan++;
        } else {
            // FFF / plain text: skip past the comma or closing quote
            scan = end + 1;
        }
        // Skip the separator: whitespace, ',', and stray '"' characters
        // (covers native AO3's ", " between anchors and FFF's ", ")
        while (scan < buffer + bufferSize &&
               (isspace(static_cast<unsigned char>(*scan)) || *scan == ',' || *scan == '"')) scan++;
    }
  }


  size_t write(uint8_t b) override {
    char c = (char)b;

    if (inSummary) {
      if (c == '<') inTag = true;
      if (!inTag && summaryBytes < 511) {
        meta.summary[summaryBytes++] = c;
      }
      if (c == '>') {
        inTag = false;
        if (bufferSize > 6 && (strstr(buffer + bufferSize - 6, "</p>") || strstr(buffer + bufferSize - 6, "</div>"))) {
          if (summaryBytes > 30) inSummary = false;
        }
      }
    }

    if (bufferSize < sizeof(buffer) - 1) {
      buffer[bufferSize++] = (char)c;
      buffer[bufferSize] = 0;
    }

    if (bufferSize > 1600) {
      processBuffer();
      const int overlap = bufferSize - 800;
      memmove(buffer, buffer + 800, overlap);
      bufferSize = overlap;
      buffer[bufferSize] = 0;
      yield();
    }
    return 1;
  }

  std::string extractDate(const char* anchor) {
    const char* pos = strstr(buffer, anchor);
    if (!pos) return "";

    const char* scan = pos + strlen(anchor);
    bool inHtm = false;
    while (scan < buffer + bufferSize) {
      if (*scan == '<') {
        inHtm = true;
        scan++;
        continue;
      }
      if (*scan == '>') {
        inHtm = false;
        scan++;
        continue;
      }
      if (inHtm) {
        scan++;
        continue;
      }
      if (isdigit(static_cast<unsigned char>(*scan))) {
        if (scan + 10 <= buffer + bufferSize) {
          bool valid = true;
          for (int i = 0; i < 10; i++) {
            if (i == 4 || i == 7) {
              if (scan[i] != '-') { valid = false; break; }
            } else {
              if (!isdigit(static_cast<unsigned char>(scan[i]))) { valid = false; break; }
            }
          }
          if (valid) {
            return std::string(scan, 10);
          }
        }
      }
      scan++;
    }
    return "";
  }

  void processBuffer() {
    if (meta.rating == '-') {
      findFuzzyField("Rating:", [this](const char* val) {
        meta.rating = Ao3Librarian::mapRating(val);
      });
    }

    if (meta.warning == 0 || meta.warning == '-') {
      findFuzzyField("Warnings:", [this](const char* val) {
        char w = Ao3Librarian::mapWarning(val);
        if (w != '-' || meta.warning == 0) meta.warning = w;
      });
      findFuzzyField("Archive Warning:", [this](const char* val) {
        char w = Ao3Librarian::mapWarning(val);
        if (w != '-' || meta.warning == 0) meta.warning = w;
      });
    }

    findFuzzyField("Words:", [this](const char* val) {
      char clean[32] = {0};
      int j = 0;
      for(int i=0; val[i] && j < 31; i++) if(isdigit(val[i])) clean[j++] = val[i];
      else if(val[i] != ',') break;
      meta.wordCount = strtoul(clean, nullptr, 10);
    });

    if (strstr(buffer, "In-Progress")) isInProgress = true;

    findFuzzyField("Chapters:", [this](const char* val) {
      const char* slash = strchr(val, '/');
      if (slash) {
        unsigned published = 0;
        for (const char* p = val; p < slash && isdigit(static_cast<unsigned char>(*p)); ++p) {
          published = published * 10u + static_cast<unsigned>(*p - '0');
          if (published > 65535u) break;
        }
        meta.chapterCount = static_cast<uint16_t>(published);
        const char* after = slash + 1;
        bool okTotal = (*after != '\0');
        for (const char* p = after; okTotal && *p; ++p) {
          if (!isdigit(static_cast<unsigned char>(*p))) okTotal = false;
        }
        unsigned total = 0;
        if (okTotal) total = static_cast<unsigned>(atoi(after));
        meta.isCompleted = okTotal && total > 0 && meta.chapterCount == static_cast<uint16_t>(total);
      } else {
        meta.chapterCount = static_cast<uint16_t>(atoi(val));
        meta.isCompleted = !isInProgress;
      }
    });

    if (strstr(buffer, "Genre:")) extractTagsFromAnchor("Genre:");
    if (!meta.tags[3][0] && strstr(buffer, "Additional Tags:")) {
      extractTagsFromAnchor("Additional Tags:");
    }
    if (strstr(buffer, "Series:")) extractSeries();

    // Fandom: native AO3 uses "Fandom:" (singular, one fandom) or "Fandoms:"
    // (plural, multiple fandoms). FFF uses "Category:".
    // IMPORTANT: native AO3 epubs also contain a "Category:" field (M/M, F/F,
    // Gen, etc.) that appears *before* the fandom label in the HTML. Always try
    // both AO3 fandom labels first — falling through to "Category:" is correct
    // only for FFF epubs, which have no "Fandom:"/"Fandoms:" label at all.
    bool foundNativeFandom = false;
    char* fandomTarget[1] = { scrapedFandom };

    if (strstr(buffer, "Fandoms:")) {
        extractCommaSeparatedFields("Fandoms:", fandomTarget, 1);
        foundNativeFandom = true;
    } else if (strstr(buffer, "Fandom:")) {
        extractCommaSeparatedFields("Fandom:", fandomTarget, 1);
        foundNativeFandom = true;
    }

    // Only look for Category if no native fandom tag was found in this specific chunk,
    // AND the destination string is completely empty.
    if (!foundNativeFandom && scrapedFandom[0] == '\0') {
        if (strstr(buffer, "Category:")) {
            extractCommaSeparatedFields("Category:", fandomTarget, 1);
        }
    }

    // Relationships: native AO3 uses "Relationships:" (plural) or "Relationship:" (singular).
    // Take up to two comma-separated values. Pass true to enable custom nickname processing.
    if (scrapedRel1[0] == '\0' && scrapedRel2[0] == '\0') {
        const char* relAnchor = nullptr;
        if (strstr(buffer, "Relationships:")) {
            relAnchor = "Relationships:";
        } else if (strstr(buffer, "Relationship:")) {
            relAnchor = "Relationship:";
        }

        if (relAnchor) {
            char* targets[2] = { scrapedRel1, scrapedRel2 };
            extractCommaSeparatedFields(relAnchor, targets, 2, true);

            for (char* rel : targets) {
                if (rel[0] == '\0') continue;

                char cleanBuf[32];
                size_t cIdx = 0;
                bool inParen = false;

                // Look at the raw string we grabbed, but process it intelligently
                for (size_t i = 0; rel[i] != '\0'; i++) {
                    if (rel[i] == '(') {
                        inParen = true;
                        // Backtrack to remove a single space before the parenthesis if present
                        if (cIdx > 0 && cleanBuf[cIdx - 1] == ' ') {
                            cIdx--;
                        }
                    } else if (rel[i] == ')') {
                        inParen = false;
                    } else if (!inParen) {
                        // The 31-character limit is evaluated here,
                        // after skipping everything inside parentheses!
                        if (cIdx < 31) {
                            // Prevent duplicate spaces caused by stripping from the middle
                            if (rel[i] == ' ' && cIdx > 0 && cleanBuf[cIdx - 1] == ' ') {
                                continue;
                            }
                            cleanBuf[cIdx++] = rel[i];
                        }
                    }
                }
                cleanBuf[cIdx] = '\0';

                // Trim any leftover trailing whitespace safely
                while (cIdx > 0 && isspace(static_cast<unsigned char>(cleanBuf[cIdx - 1]))) {
                    cleanBuf[--cIdx] = '\0';
                }

                // Copy the isolated, clean string back into the persistent variable
                strcpy(rel, cleanBuf);
            }
        }
    }

    const char* sumPos = strstr(buffer, "Summary:");
    if (sumPos && !inSummary && meta.summary[0] == '\0') {
      const char* scan = sumPos + (sizeof("Summary:") - 1);
      bool htm = false;
      while (scan < buffer + bufferSize) {
        if (*scan == '<') htm = true;
        else if (*scan == '>') htm = false;
        else if (!htm && !isspace(static_cast<unsigned char>(*scan)) && *scan != ':' && *scan != '-' && *scan != '/') {
            inSummary = true;
            while (scan < buffer + bufferSize && summaryBytes < 511) {
                if (*scan == '<') htm = true;
                else if (*scan == '>') htm = false;
                else if (!htm) meta.summary[summaryBytes++] = *scan;
                scan++;
            }
            break;
        }
        scan++;
      }
    }

    if (meta.summary[0] == '\0') tryExtractNativeSummary();

    // Native AO3: extract work ID from works/ URL in HTML
    const char* pos = strstr(buffer, "archiveofourown.org/works/");
    if (pos) {
      const char* p = pos + 26;
      std::string extractedId = "";
      while (*p && isdigit(static_cast<unsigned char>(*p))) {
        extractedId += *p;
        p++;
      }
      if (extractedId.size() > scrapedWorkId.size()) {
        scrapedWorkId = extractedId;
      }
    }

    // Extract last updated or published date with Updated having absolute precedence
    std::string tempDate = extractDate("Updated:");
    if (!tempDate.empty()) {
      scrapedDate = tempDate;
      hasUpdatedDate = true;
    } else if (!hasUpdatedDate) {
      tempDate = extractDate("Published:");
      if (!tempDate.empty()) {
        scrapedDate = tempDate;
      }
    }
  }

  void tryExtractNativeSummary() {
    char candidate[sizeof(meta.summary)] = {};
    const size_t extracted =
        Ao3HtmlMetadataParser::extractSummary(buffer, bufferSize, candidate, sizeof(candidate));
    if (extracted > strlen(meta.summary)) {
      memcpy(meta.summary, candidate, extracted + 1);
      summaryBytes = extracted;
    }
  }

  void findFuzzyField(const char* anchor, std::function<void(const char*)> callback) {
    const char* pos = strstr(buffer, anchor);
    if (!pos) return;

    const char* scan = pos + strlen(anchor);
    bool inHtm = false;
    const char* valStart = nullptr;

    while (scan < buffer + bufferSize) {
      if (*scan == '<') { inHtm = true; scan++; continue; }
      if (*scan == '>') { inHtm = false; scan++; continue; }
      if (!inHtm && !isspace(*scan) && *scan != ':' && *scan != '-' && *scan != '/' && *scan != 's' && *scan != 'S') {
        valStart = scan;
        break;
      }
      scan++;
    }

    if (!valStart) return;

    const char* valEnd = strchr(valStart, '<');
    if (valEnd) {
      char temp[128];
      size_t len = std::min((size_t)(valEnd - valStart), sizeof(temp) - 1);
      strncpy(temp, valStart, len);
      temp[len] = 0;
      while (len > 0 && isspace(temp[len-1])) temp[--len] = 0;
      callback(temp);
    }
  }

  void extractSeries() {
    const char* pos = strstr(buffer, "Series:");
    if (!pos) return;

    const char* scan = pos + (sizeof("Series:") - 1);
    // Skip to the start of the value
    while (scan < buffer + bufferSize) {
      if (*scan == '<') {
        while (scan < buffer + bufferSize && *scan != '>') scan++;
        if (scan < buffer + bufferSize) scan++;
      } else if (isspace(*scan) || *scan == ':' || *scan == '/' || *scan == '-' || *scan == '_') {
        scan++;
      } else {
        break;
      }
    }

    if (scan >= buffer + bufferSize) return;

    // Read characters, ignoring HTML tags, until we hit a newline or EOF.
    char seriesText[256];
    size_t outIdx = 0;
    while (scan < buffer + bufferSize && *scan != '\n' && *scan != '\r' && outIdx < sizeof(seriesText) - 1) {
      if (*scan == '<') {
        while (scan < buffer + bufferSize && *scan != '>') scan++;
      } else if (*scan != '>') {
        seriesText[outIdx++] = *scan;
      }
      scan++;
    }
    seriesText[outIdx] = 0;

    // Clean up trailing spaces
    while (outIdx > 0 && isspace(seriesText[outIdx - 1])) {
      seriesText[--outIdx] = 0;
    }

    // Native AO3: "part N of Series Title"
    {
      char* p = seriesText;
      while (*p && isspace(static_cast<unsigned char>(*p))) p++;
      if (strncasecmp(p, "part ", 5) == 0) {
        const char* num = p + 5;
        int part = atoi(num);
        while (*num && isdigit(static_cast<unsigned char>(*num))) num++;
        while (*num && isspace(static_cast<unsigned char>(*num))) num++;
        if (strncasecmp(num, "of ", 3) == 0) {
          num += 3;
          while (*num && isspace(static_cast<unsigned char>(*num))) num++;
          if (part > 0 && *num) {
            meta.seriesPart = static_cast<uint16_t>(part);
            strncpy(meta.seriesName, num, sizeof(meta.seriesName) - 1);
            meta.seriesName[sizeof(meta.seriesName) - 1] = '\0';
            return;
          }
        }
      }
    }

    // FanFicFare: "Title [N]"
    char* bracketPos = strrchr(seriesText, '[');
    if (bracketPos) {
      int part = atoi(bracketPos + 1);
      if (part > 0) meta.seriesPart = part;

      *bracketPos = 0; // Terminate name before bracket
      size_t nameLen = strlen(seriesText);
      while (nameLen > 0 && isspace(seriesText[nameLen - 1])) {
        seriesText[--nameLen] = 0;
      }
      strncpy(meta.seriesName, seriesText, sizeof(meta.seriesName) - 1);
      meta.seriesName[sizeof(meta.seriesName) - 1] = '\0';
    } else {
      strncpy(meta.seriesName, seriesText, sizeof(meta.seriesName) - 1);
      meta.seriesName[sizeof(meta.seriesName) - 1] = '\0';
    }
  }

  void extractTagsFromAnchor(const char* anchor) {
    Ao3HtmlMetadataParser::extractTags(buffer, bufferSize, anchor, meta.tags);
  }
};

} // namespace

bool Ao3Librarian::scrape(const Epub& epub, bool force) {
  const std::string infoPath = epub.getCachePath() + "/ao3_library_info";

  if (!force && Storage.exists(infoPath.c_str())) {
    auto existing = std::unique_ptr<Ao3LibraryMetadata>(new Ao3LibraryMetadata());
    if (getLibraryInfo(epub, *existing)) {
      if (existing->version == 8 && existing->rating != '-' && existing->warning != 0 && existing->summary[0] != 0 && existing->wordCount > 0) {
        // If ao3-info.bin is missing but we already have cached library info,
        // we can still attempt to rebuild ao3-info.bin if we sniff a native preface
        if (!epub.hasAo3Info() && sniffNativeAo3Preface(epub)) {
          auto meta = std::unique_ptr<Ao3LibraryMetadata>(new Ao3LibraryMetadata());
          std::string scrapedWorkId = "";
          std::string scrapedDate = "";
          char tempFandom[32] = {};
          char tempRel1[32] = {};
          char tempRel2[32] = {};
          if (parseTitlePage(epub, *meta, scrapedWorkId, scrapedDate, tempFandom, tempRel1, tempRel2)) {
            if (!scrapedWorkId.empty()) {
              epub.saveAo3Info(scrapedWorkId, scrapedDate, meta->isCompleted);
            }
          }
        }
        return true;
      }
    }
  }

  auto meta = std::unique_ptr<Ao3LibraryMetadata>(new Ao3LibraryMetadata());
  std::string scrapedWorkId = "";
  std::string scrapedDate = "";
  char scrapedFandom[32] = {};
  char scrapedRel1[32]   = {};
  char scrapedRel2[32]   = {};

  if (!parseTitlePage(epub, *meta, scrapedWorkId, scrapedDate, scrapedFandom, scrapedRel1, scrapedRel2)) return false;

  strncpy(meta->filepath, epub.getPath().c_str(), 255);

  // Clean up "&amp;" in the title
  std::string title = epub.getTitle();
  size_t titlePos = 0;
  while ((titlePos = title.find("&amp;", titlePos)) != std::string::npos) {
    title.replace(titlePos, 5, "&");
    titlePos += 1; // Advance past the newly inserted '&'
  }
  strncpy(meta->title, title.c_str(), 127);
  meta->title[127] = '\0';

  // Clean up "&amp;" in the author name (covers co-authored fics!)
  std::string author = epub.getAuthor();
  size_t authorPos = 0;
  while ((authorPos = author.find("&amp;", authorPos)) != std::string::npos) {
    author.replace(authorPos, 5, "&");
    authorPos += 1;
  }
  strncpy(meta->author, author.c_str(), 127);
  meta->author[127] = '\0';

  strncpy(meta->updatedDate, scrapedDate.c_str(), sizeof(meta->updatedDate) - 1);
  meta->updatedDate[sizeof(meta->updatedDate) - 1] = '\0';

  // Generate the ao3-info.bin sidecar if missing and we extracted a valid Work ID
  if (!epub.hasAo3Info() && !scrapedWorkId.empty()) {
    epub.saveAo3Info(scrapedWorkId, scrapedDate, meta->isCompleted);
  }

  FsFile f;
  if (Storage.openFileForWrite("AO3L", infoPath, f)) {
    f.write((uint8_t*)meta.get(), sizeof(Ao3LibraryMetadata));
    f.close();

    CompactIndexRecord rec;
    memset(&rec, 0, sizeof(rec));

    strncpy(rec.title,         meta->title,      63);
    strncpy(rec.author,        meta->author,     31);
    strncpy(rec.seriesName,    meta->seriesName, 31);
    strncpy(rec.fandom,        scrapedFandom,    31);
    strncpy(rec.relationship1, scrapedRel1,      31);
    strncpy(rec.relationship2, scrapedRel2,      31);
    rec.wordCount  = meta->wordCount;
    rec.seriesPart = meta->seriesPart;
    rec.cacheHash  = ao3PathHash(epub.getPath());
    rec.flags      = withAo3Rating(rec.flags, meta->rating);

    if (!writeIndexRecord(rec)) {
        return false;
    }

    return true;
  }
  return false;
}

bool Ao3Librarian::getLibraryInfo(const Epub& epub, Ao3LibraryMetadata& meta) {
  const std::string infoPath = epub.getCachePath() + "/ao3_library_info";
  const bool loaded = readAo3LibraryInfoAtPath(infoPath, meta);
  if (loaded) rememberInfoPath(ao3PathHash(epub.getPath()), infoPath);
  return loaded;
}

bool Ao3Librarian::getLibraryInfo(const std::string& epubPath, Ao3LibraryMetadata& meta) {
  const std::string infoPath = Epub::cachePathForFilePath(epubPath, AO3_CACHE_ROOT) + "/ao3_library_info";
  const bool loaded = readAo3LibraryInfoAtPath(infoPath, meta);
  if (loaded) rememberInfoPath(ao3PathHash(epubPath), infoPath);
  return loaded;
}

bool Ao3Librarian::parseTitlePage(const Epub& epub,
                                  Ao3LibraryMetadata& meta,
                                  std::string& scrapedWorkId,
                                  std::string& scrapedDate,
                                  char* scrapedFandom,
                                  char* scrapedRel1,
                                  char* scrapedRel2) {
  if (epub.getSpineItemsCount() == 0) return false;

  // Check up to the first 3 spine items to account for internal cover pages
  int itemsToCheck = std::min(3, epub.getSpineItemsCount());
  bool foundInfoSpine = false;

  for (int i = 0; i < itemsToCheck; i++) {
    std::string href = epub.getSpineItem(i).href;
    auto scraper = std::unique_ptr<HtmlScraper>(new HtmlScraper(meta));

    if (epub.readItemContentsToStream(href, *scraper, 8192)) {
      scraper->processBuffer();

      // workId/date may be in a different spine (e.g. native AO3 preface) —
      // propagate from whichever spine finds them first, as before.
      if (scrapedWorkId.empty() && !scraper->scrapedWorkId.empty()) {
        scrapedWorkId = scraper->scrapedWorkId;
      }
      if (scrapedDate.empty() && !scraper->scrapedDate.empty()) {
        scrapedDate = scraper->scrapedDate;
      }

      // Fandom/relationship: take both from the FIRST spine where fandom
      // is found, as a unit — including an empty relationship as final.
      if (!foundInfoSpine && scraper->scrapedFandom[0] != '\0') {
        foundInfoSpine = true;
        strncpy(scrapedFandom, scraper->scrapedFandom, 31);
        scrapedFandom[31] = '\0';
        strncpy(scrapedRel1, scraper->scrapedRel1, 31);
        scrapedRel1[31] = '\0';
        strncpy(scrapedRel2, scraper->scrapedRel2, 31);
        scrapedRel2[31] = '\0';
      }
    }
  }

  return meta.wordCount > 0;
}

bool Ao3Librarian::sniffNativeAo3Preface(const Epub& epub) {
  return epub.sniffNativeAo3Preface();
}

char Ao3Librarian::mapRating(const char* s) {
  if (containsCaseInsensitive(s, "General")) return 'G';
  if (containsCaseInsensitive(s, "Teen")) return 'T';
  if (containsCaseInsensitive(s, "Mature")) return 'M';
  if (containsCaseInsensitive(s, "Explicit")) return 'E';
  return '-';
}

char Ao3Librarian::mapWarning(const char* s) {
  if (containsCaseInsensitive(s, "Creator Chose Not To Use") ||
      containsCaseInsensitive(s, "Choose Not To Use Archive Warnings")) {
    return 'B';
  }
  if (containsCaseInsensitive(s, "No Archive Warnings Apply")) return '-';
  if (strlen(s) > 3) return '!';
  return '-';
}

void Ao3Librarian::forEachLibraryInfo(const std::function<void(const Ao3LibraryMetadata&)>& callback) {
  if (!callback) return;

  forEachAo3InfoSidecar([&callback](const std::string& infoPath) {
    Ao3LibraryMetadata meta;
    if (readAo3LibraryInfoAtPath(infoPath, meta)) {
      callback(meta);
    }
  });
}

void Ao3Librarian::forEachLibraryInfoWhile(const std::function<bool(const Ao3LibraryMetadata&)>& callback) {
  if (!callback) return;

  bool done = false;
  forEachAo3InfoSidecar([&callback, &done](const std::string& infoPath) {
    Ao3LibraryMetadata meta;
    if (readAo3LibraryInfoAtPath(infoPath, meta) && !callback(meta)) done = true;
  }, &done);
}

bool Ao3Librarian::findLibraryInfoByCacheHash(const uint32_t cacheHash, Ao3LibraryMetadata& meta) {
  bool found = false;
  findLibraryInfoByCacheHashes(&cacheHash, 1, &meta, &found);
  return found;
}

void Ao3Librarian::findLibraryInfoByCacheHashes(const uint32_t* cacheHashes, const size_t count,
                                                Ao3LibraryMetadata* metadata, bool* found) {
  if (!cacheHashes || !metadata || !found || count == 0) return;

  for (size_t i = 0; i < count; ++i) found[i] = false;
  size_t remaining = count;
  for (size_t i = 0; i < count; ++i) {
    const auto cached = std::find_if(recentInfoPaths.begin(), recentInfoPaths.end(),
                                     [hash = cacheHashes[i]](const RecentInfoPath& item) {
                                       return item.cacheHash == hash;
                                     });
    if (cached != recentInfoPaths.end() && readAo3LibraryInfoAtPath(cached->infoPath, metadata[i])) {
      found[i] = true;
      remaining--;
    }
  }
  if (remaining == 0) return;

  for (const uint64_t fullHash : infoLocators) {
    const uint32_t cacheHash = static_cast<uint32_t>(fullHash);
    for (size_t i = 0; i < count; ++i) {
      if (found[i] || cacheHashes[i] != cacheHash) continue;
      const std::string infoPath = infoPathForHash(fullHash);
      Ao3LibraryMetadata candidate;
      if (!readAo3LibraryInfoAtPath(infoPath, candidate) || ao3PathHash(candidate.filepath) != cacheHashes[i]) {
        continue;
      }
      metadata[i] = candidate;
      found[i] = true;
      rememberInfoPath(cacheHashes[i], infoPath);
      if (--remaining == 0) return;
    }
  }

  bool done = false;
  forEachAo3InfoSidecar([&](const std::string& infoPath) {
    // Cache directory names contain the full path hash. Reject unrelated
    // directories before opening their sidecars; AO3 library paging and
    // end-of-series lookup otherwise read every cached EPUB on the card.
    uint32_t directoryHash = 0;
    if (cacheHashFromInfoPath(infoPath, directoryHash)) {
      bool requested = false;
      for (size_t i = 0; i < count; ++i) {
        if (!found[i] && cacheHashes[i] == directoryHash) {
          requested = true;
          break;
        }
      }
      if (!requested) return;
    }

    Ao3LibraryMetadata candidate;
    if (!readAo3LibraryInfoAtPath(infoPath, candidate) || candidate.filepath[0] == '\0') return;
    const uint32_t candidateHash = ao3PathHash(candidate.filepath);
    for (size_t i = 0; i < count; ++i) {
      if (!found[i] && cacheHashes[i] == candidateHash) {
        metadata[i] = candidate;
        found[i] = true;
        rememberInfoPath(candidateHash, infoPath);
        remaining--;
        done = remaining == 0;
        break;
      }
    }
  }, &done);
}

bool Ao3Librarian::hasAnyAo3Fics() {
  bool found = false;
  forEachAo3InfoSidecar([&found](const std::string& infoPath) {
    (void)infoPath;
    if (!found) {
      found = true;
    }
  }, &found);
  return found;
}

Ao3LibrarySummary Ao3Librarian::getLibrarySummary() {
  if (librarySummaryValid) return cachedLibrarySummary;

  Ao3LibrarySummary summary;
  forEachAo3InfoSidecar([&summary](const std::string& infoPath) {
    Ao3LibraryMetadata meta;
    if (!readAo3LibraryInfoAtPath(infoPath, meta) || meta.filepath[0] == '\0' ||
        !Storage.exists(meta.filepath)) {
      return;
    }

    if (summary.total < UINT16_MAX) summary.total++;
    const std::string cachePath = Epub::cachePathForFilePath(meta.filepath, AO3_CACHE_ROOT);
    switch (Ao3ReadingStateStore::load(cachePath)) {
      case Ao3ReadingState::WaitingForChapter:
        if (summary.waiting < UINT16_MAX) summary.waiting++;
        break;
      case Ao3ReadingState::UpdateAvailable:
        if (summary.updatesAvailable < UINT16_MAX) summary.updatesAvailable++;
        break;
      case Ao3ReadingState::None:
        break;
    }
  });
  cachedLibrarySummary = summary;
  librarySummaryValid = true;
  return cachedLibrarySummary;
}

std::vector<std::string> Ao3Librarian::findNextSeriesBooks(const std::string& epubPath, const size_t maxCount) {
  std::vector<std::string> result;
  if (maxCount == 0) return result;

  Ao3LibraryMetadata current;
  if (!getLibraryInfo(epubPath, current) || current.seriesName[0] == '\0' || current.seriesPart == 0) {
    return result;
  }

  struct Candidate {
    uint16_t part;
    uint32_t cacheHash;
  };
  std::vector<Candidate> candidates;
  candidates.reserve(maxCount + 1);

  HalFile index;
  uint16_t recordCount = 0;
  if (!Storage.openFileForRead("AO3L", AO3_INDEX_PATH, index) || !readIndexHeader(index, recordCount)) {
    if (index) index.close();
    return result;
  }

  CompactIndexRecord record;
  for (uint16_t i = 0; i < recordCount; ++i) {
    index.seek(offsetOf(i));
    if (index.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) != sizeof(record)) break;
    // Compact records intentionally truncate series names. Match their stored
    // prefix here, then verify the complete name after resolving the sidecar.
    if ((record.flags & 1) || record.seriesPart <= current.seriesPart ||
        strncasecmp(record.seriesName, current.seriesName, sizeof(record.seriesName) - 1) != 0) {
      continue;
    }
    const bool duplicate = std::any_of(candidates.begin(), candidates.end(), [&](const Candidate& candidate) {
      return candidate.cacheHash == record.cacheHash;
    });
    if (duplicate) continue;

    candidates.push_back({record.seriesPart, record.cacheHash});
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
      if (a.part != b.part) return a.part < b.part;
      return a.cacheHash < b.cacheHash;
    });
    if (candidates.size() > maxCount) candidates.pop_back();
  }
  index.close();

  if (candidates.empty()) return result;

  std::vector<uint32_t> hashes;
  hashes.reserve(candidates.size());
  for (const Candidate& candidate : candidates) hashes.push_back(candidate.cacheHash);
  std::vector<Ao3LibraryMetadata> metadata(candidates.size());
  std::unique_ptr<bool[]> found(new bool[candidates.size()]);
  findLibraryInfoByCacheHashes(hashes.data(), hashes.size(), metadata.data(), found.get());

  result.reserve(candidates.size());
  for (size_t i = 0; i < candidates.size(); ++i) {
    if (!found[i] || metadata[i].filepath[0] == '\0' || !Storage.exists(metadata[i].filepath) ||
        metadata[i].seriesPart <= current.seriesPart ||
        strcasecmp(metadata[i].seriesName, current.seriesName) != 0) {
      continue;
    }
    const std::string path = metadata[i].filepath;
    if (std::find(result.begin(), result.end(), path) == result.end()) result.push_back(path);
  }
  return result;
}

void Ao3Librarian::invalidateSummaryCache() { librarySummaryValid = false; }

void Ao3Librarian::updateSummaryForStateChange(const Ao3ReadingState oldState, const Ao3ReadingState newState) {
  if (!librarySummaryValid || oldState == newState) return;
  const auto decrement = [](uint16_t& value) {
    if (value > 0) --value;
  };
  if (oldState == Ao3ReadingState::WaitingForChapter) decrement(cachedLibrarySummary.waiting);
  if (oldState == Ao3ReadingState::UpdateAvailable) decrement(cachedLibrarySummary.updatesAvailable);
  if (newState == Ao3ReadingState::WaitingForChapter && cachedLibrarySummary.waiting < UINT16_MAX) {
    ++cachedLibrarySummary.waiting;
  }
  if (newState == Ao3ReadingState::UpdateAvailable && cachedLibrarySummary.updatesAvailable < UINT16_MAX) {
    ++cachedLibrarySummary.updatesAvailable;
  }
}

bool Ao3Librarian::writeIndexRecord(const CompactIndexRecord& rec) {
    // --- Validate existing file ---
    bool needsCreate = false;
    if (!Storage.exists(AO3_INDEX_PATH)) {
        needsCreate = true;
    } else {
        HalFile check;
        if (Storage.openFileForRead("AO3L", AO3_INDEX_PATH, check)) {
            uint16_t recordCountCheck = 0;
            const bool readOk = readIndexHeader(check, recordCountCheck);
            check.close();
            if (!readOk) {
                Storage.remove(AO3_INDEX_PATH);
                needsCreate = true;
            }
        } else {
            needsCreate = true;
        }
    }

    // --- Create fresh file with empty header if needed ---
    if (needsCreate) {
        invalidateSummaryCache();
        HalFile f;
        if (!Storage.openFileForWrite("AO3L", AO3_INDEX_PATH, f)) return false;
        uint8_t  v = 1,  r = 0;
        uint16_t c = 0;
        uint32_t s = 0;
        f.write((uint8_t*)"AO3X", 4);
        f.write(&v, 1);
        f.write((uint8_t*)&c, 2);
        f.write((uint8_t*)&s, 4);
        f.write(&r, 1);
        f.close();
    }

    // --- Open for read/write ---
    HalFile f = Storage.open(AO3_INDEX_PATH, O_RDWR);
    if (!f) return false;

    uint16_t recordCount = 0;
    uint32_t nextSequence = 0;
    if (!readIndexHeader(f, recordCount, &nextSequence)) {
        f.close();
        return false;
    }

    int32_t  updateSlot     = -1;
    uint32_t preservedSeq   = 0;
    int32_t  freeSlot       = -1;
    uint16_t liveCount      = 0;
    CompactIndexRecord existing;
    for (uint16_t i = 0; i < recordCount; i++) {
        if (f.read((uint8_t*)&existing, sizeof(existing)) != sizeof(existing)) {
            break;
        }
        if (existing.flags & 1) {
            if (freeSlot < 0) freeSlot = i;
        } else {
            liveCount++;
            if (existing.cacheHash == rec.cacheHash) {
                updateSlot   = i;
                preservedSeq = existing.addedSequence;
            }
        }
    }

    CompactIndexRecord recToWrite = rec;

    if (updateSlot >= 0) {
        recToWrite.addedSequence = preservedSeq;
        f.seek(offsetOf(updateSlot));
        f.write((uint8_t*)&recToWrite, sizeof(recToWrite));
        f.close();
        return true;
    }

    recToWrite.addedSequence = nextSequence;
    nextSequence++;
    invalidateSummaryCache();

    if (freeSlot >= 0) {
        f.seek(offsetOf(freeSlot));
        f.write((uint8_t*)&recToWrite, sizeof(recToWrite));
    } else {
        if (liveCount >= MAX_LIBRARY_BOOKS) {
            f.close();
            return false;
        }
        f.seek(f.size());
        f.write((uint8_t*)&recToWrite, sizeof(recToWrite));
        recordCount++;
        f.seek(5);
        f.write((uint8_t*)&recordCount, 2);
    }

    f.seek(7);
    f.write((uint8_t*)&nextSequence, 4);

    f.close();
    return true;
}

bool Ao3Librarian::tombstoneRecord(const std::string& epubPath) {
    invalidateSummaryCache();
    recentInfoPaths.clear();
    infoLocators.clear();
    if (!Storage.exists(AO3_INDEX_PATH)) return false;

    HalFile f = Storage.open(AO3_INDEX_PATH, O_RDWR);
    if (!f) return false;

    uint16_t recordCount = 0;
    if (!readIndexHeader(f, recordCount)) {
        f.close();
        return false;
    }

    uint32_t targetHash = ao3PathHash(epubPath);

    CompactIndexRecord rec;
    for (uint16_t i = 0; i < recordCount; i++) {
        f.seek(offsetOf(i));
        if (f.read((uint8_t*)&rec, sizeof(rec)) != sizeof(rec)) break;

        if (!(rec.flags & 1) && rec.cacheHash == targetHash) {
            rec.flags |= 1;
            f.seek(offsetOf(i));
            f.write((uint8_t*)&rec, sizeof(rec));
            f.close();
            return true;
        }
    }

    f.close();
    return false;
}

bool Ao3Librarian::migratePath(const std::string& oldPath, const std::string& newPath) {
    recentInfoPaths.clear();
    infoLocators.clear();

    const std::string infoPath = Epub::cachePathForFilePath(oldPath, AO3_CACHE_ROOT) + "/ao3_library_info";
    Ao3LibraryMetadata metadata;
    if (!readAo3LibraryInfoAtPath(infoPath, metadata)) {
        return true;
    }

    memset(metadata.filepath, 0, sizeof(metadata.filepath));
    strncpy(metadata.filepath, newPath.c_str(), sizeof(metadata.filepath) - 1);
    HalFile infoFile;
    if (!Storage.openFileForWrite("AO3L", infoPath, infoFile)) {
        return false;
    }
    const bool sidecarUpdated =
        infoFile.write(reinterpret_cast<const uint8_t*>(&metadata), sizeof(metadata)) == sizeof(metadata);
    infoFile.close();
    if (!sidecarUpdated) {
        return false;
    }

    if (!Storage.exists(AO3_INDEX_PATH)) {
        return true;
    }
    HalFile index = Storage.open(AO3_INDEX_PATH, O_RDWR);
    if (!index) {
        return false;
    }
    uint16_t recordCount = 0;
    if (!readIndexHeader(index, recordCount)) {
        index.close();
        return false;
    }

    const uint32_t oldHash = ao3PathHash(oldPath);
    const uint32_t newHash = ao3PathHash(newPath);
    CompactIndexRecord record;
    for (uint16_t i = 0; i < recordCount; ++i) {
        index.seek(offsetOf(i));
        if (index.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) != sizeof(record)) break;
        if (!(record.flags & 1) && record.cacheHash == oldHash) {
            record.cacheHash = newHash;
            index.seek(offsetOf(i));
            const bool updated = index.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record)) == sizeof(record);
            index.close();
            return updated;
        }
    }
    index.close();
    return true;
}

int Ao3Librarian::sanitizeIndex() {
    invalidateSummaryCache();
    recentInfoPaths.clear();
    infoLocators.clear();
    if (!Storage.exists(AO3_INDEX_PATH)) return 0;

    HalFile f = Storage.open(AO3_INDEX_PATH, O_RDWR);
    if (!f) return -1;

    uint16_t recordCount = 0;
    if (!readIndexHeader(f, recordCount)) {
        f.close();
        return -1;
    }

    std::vector<uint32_t> validHashes;
    validHashes.reserve(recordCount);
    forEachAo3InfoSidecar([&validHashes](const std::string& infoPath) {
        Ao3LibraryMetadata meta;
        if (readAo3LibraryInfoAtPath(infoPath, meta) && meta.filepath[0] != '\0' && Storage.exists(meta.filepath)) {
            validHashes.push_back(ao3PathHash(meta.filepath));
        }
    });

    int tombstoned = 0;
    CompactIndexRecord rec;

    for (uint16_t i = 0; i < recordCount; i++) {
        f.seek(offsetOf(i));
        if (f.read((uint8_t*)&rec, sizeof(rec)) != sizeof(rec)) break;

        if (rec.flags & 0x01) continue; // already tombstoned

        const bool valid = std::find(validHashes.begin(), validHashes.end(), rec.cacheHash) != validHashes.end();

        if (!valid) {
            rec.flags |= 0x01;
            f.seek(offsetOf(i));
            f.write((uint8_t*)&rec, sizeof(rec));
            tombstoned++;
            LOG_DBG("AO3L", "sanitizeIndex: tombstoned ghost record (hash %u)", rec.cacheHash);
        }

        yield();
    }

    f.close();
    if (tombstoned > 0) {
        LOG_INF("AO3L", "sanitizeIndex: removed %d ghost record(s)", tombstoned);
    }
    return tombstoned;
}
