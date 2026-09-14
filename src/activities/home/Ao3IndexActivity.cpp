#include "Ao3IndexActivity.h"

#include <Arduino.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <ZipFile.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "Ao3CompactIndexRecord.h"
#include "Ao3Librarian.h"
#include "MappedInputManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr char AO3_INDEX_PATH[] = "/.crosspoint/ao3_library_index.bin";
constexpr uint8_t AO3_INDEX_VERSION = 1;

bool readIndexHeader(HalFile& file, uint16_t& recordCount) {
  char magic[4];
  uint8_t version = 0;
  uint32_t sequence = 0;
  uint8_t reserved = 0;
  return file.read(magic, 4) == 4 && file.read(&version, 1) == 1 &&
         file.read(reinterpret_cast<uint8_t*>(&recordCount), 2) == 2 &&
         file.read(reinterpret_cast<uint8_t*>(&sequence), 4) == 4 && file.read(&reserved, 1) == 1 &&
         memcmp(magic, "AO3X", 4) == 0 && version == AO3_INDEX_VERSION && recordCount <= MAX_LIBRARY_BOOKS;
}

bool isAo3Publisher(std::string publisher) {
  std::transform(publisher.begin(), publisher.end(), publisher.begin(), [](const unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return publisher == "archive of our own" || publisher.find("archiveofourown") != std::string::npos;
}
}  // namespace

Ao3IndexActivity::Ao3IndexActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                   std::string scanRoot, const int batchSize,
                                   std::vector<std::string> ignoredFolders, const bool refreshExisting)
    : Activity("Ao3Index", renderer, mappedInput),
      scanRoot(std::move(scanRoot)),
      ignoredFolders(std::move(ignoredFolders)),
      batchSize(std::clamp(batchSize, 1, 50)),
      refreshExisting(refreshExisting) {
  while (this->scanRoot.length() > 1 && this->scanRoot.back() == '/') this->scanRoot.pop_back();
  for (std::string& path : this->ignoredFolders) {
    while (path.size() > 1 && path.back() == '/') path.pop_back();
  }
  std::sort(this->ignoredFolders.begin(), this->ignoredFolders.end());
  this->ignoredFolders.erase(std::unique(this->ignoredFolders.begin(), this->ignoredFolders.end()),
                             this->ignoredFolders.end());
}

uint32_t Ao3IndexActivity::pathHash(const std::string& path) {
  return static_cast<uint32_t>(ZipFile::fnvHash64(path.c_str(), path.size()));
}

bool Ao3IndexActivity::isEpubName(std::string name) {
  const size_t dot = name.find_last_of('.');
  if (dot == std::string::npos) return false;
  name = name.substr(dot + 1);
  std::transform(name.begin(), name.end(), name.begin(), [](const unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return name == "epub";
}

bool Ao3IndexActivity::alreadyHandled(const uint32_t hash) const {
  return (!refreshExisting && std::binary_search(indexedHashes.begin(), indexedHashes.end(), hash)) ||
         std::binary_search(attemptedHashes.begin(), attemptedHashes.end(), hash);
}

bool Ao3IndexActivity::isIgnored(const std::string& path) const {
  return std::any_of(ignoredFolders.begin(), ignoredFolders.end(), [&path](const std::string& ignored) {
    return path == ignored ||
           (path.size() > ignored.size() && path.compare(0, ignored.size(), ignored) == 0 &&
            path[ignored.size()] == '/');
  });
}

bool Ao3IndexActivity::isUnderScanRoot(const std::string& path) const {
  return scanRoot == "/" || path == scanRoot ||
         (path.size() > scanRoot.size() && path.compare(0, scanRoot.size(), scanRoot) == 0 &&
          path[scanRoot.size()] == '/');
}

void Ao3IndexActivity::buildIndexedHashes() {
  indexedHashes.clear();
  HalFile file;
  if (!Storage.openFileForRead("AO3I", AO3_INDEX_PATH, file)) return;
  uint16_t count = 0;
  if (!readIndexHeader(file, count)) {
    file.close();
    return;
  }
  for (uint16_t i = 0; i < count; ++i) {
    CompactIndexRecord record;
    if (file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) != sizeof(record)) break;
    if (!(record.flags & 1)) indexedHashes.push_back(record.cacheHash);
  }
  file.close();
  std::sort(indexedHashes.begin(), indexedHashes.end());
}

void Ao3IndexActivity::onEnter() {
  Activity::onEnter();
  state = State::Discovering;
  unindexedCount = indexedCount = failedCount = currentBook = 0;
  pendingBooks.clear();
  attemptedHashes.clear();
  directories.clear();
  buildIndexedHashes();
  if (!refreshExisting && indexedHashes.size() >= MAX_LIBRARY_BOOKS) {
    state = State::Error;
    errorMessage = "AO3 library is full (400 works).";
  } else if (scanRoot.empty() || !Storage.exists(scanRoot.c_str())) {
    state = State::Error;
    errorMessage = "Choose a dedicated AO3 folder before indexing.";
  } else {
    if (!refreshExisting) directories.push_back({scanRoot, 0});
  }
  requestUpdate(true);
}

void Ao3IndexActivity::discoverRefreshCandidates() {
  unindexedCount = 0;
  Ao3Librarian::forEachLibraryInfo([this](const Ao3LibraryMetadata& metadata) {
    const std::string path = metadata.filepath;
    if (!path.empty() && isUnderScanRoot(path) && !isIgnored(path) && Storage.exists(path.c_str())) {
      ++unindexedCount;
    }
  });
  state = unindexedCount > 0 ? State::Confirm : State::Complete;
}

void Ao3IndexActivity::discoverNextDirectory() {
  if (directories.empty()) {
    state = unindexedCount > 0 ? State::Confirm : State::Complete;
    requestUpdate(true);
    return;
  }
  DirectoryEntry current = std::move(directories.back());
  directories.pop_back();
  if (isIgnored(current.path)) return;
  HalFile directory = Storage.open(current.path.c_str());
  if (!directory || !directory.isDirectory()) {
    if (directory) directory.close();
    return;
  }

  HalFile child;
  char name[256];
  while ((child = directory.openNextFile())) {
    child.getName(name, sizeof(name));
    std::string path = current.path;
    if (path.back() != '/') path += '/';
    path += name;
    if (child.isDirectory()) {
      if (name[0] != '.' && strcmp(name, "System Volume Information") != 0 && current.depth < 6) {
        if (!isIgnored(path)) directories.push_back({std::move(path), static_cast<uint8_t>(current.depth + 1)});
      }
    } else if (isEpubName(name) && !alreadyHandled(pathHash(path))) {
      ++unindexedCount;
    }
    child.close();
  }
  directory.close();
  yield();
  requestUpdate();
}

void Ao3IndexActivity::collectNextBatch() {
  buildIndexedHashes();
  pendingBooks.clear();
  if (refreshExisting) {
    Ao3Librarian::forEachLibraryInfoWhile([this](const Ao3LibraryMetadata& metadata) {
      const std::string path = metadata.filepath;
      if (path.empty() || !isUnderScanRoot(path) || isIgnored(path) || !Storage.exists(path.c_str()) ||
          alreadyHandled(pathHash(path))) {
        return true;
      }
      pendingBooks.push_back(path);
      return static_cast<int>(pendingBooks.size()) < batchSize;
    });
    currentBook = 0;
    state = pendingBooks.empty() ? State::Complete : State::Indexing;
    requestUpdate(true);
    return;
  }
  directories.clear();
  directories.push_back({scanRoot, 0});
  while (!directories.empty() && static_cast<int>(pendingBooks.size()) < batchSize) {
    DirectoryEntry current = std::move(directories.back());
    directories.pop_back();
    if (isIgnored(current.path)) continue;
    HalFile directory = Storage.open(current.path.c_str());
    if (!directory || !directory.isDirectory()) {
      if (directory) directory.close();
      continue;
    }
    HalFile child;
    char name[256];
    while ((child = directory.openNextFile())) {
      child.getName(name, sizeof(name));
      std::string path = current.path;
      if (path.back() != '/') path += '/';
      path += name;
      if (child.isDirectory()) {
        if (name[0] != '.' && strcmp(name, "System Volume Information") != 0 && current.depth < 6) {
          if (!isIgnored(path)) directories.push_back({std::move(path), static_cast<uint8_t>(current.depth + 1)});
        }
      } else if (isEpubName(name) && !alreadyHandled(pathHash(path))) {
        pendingBooks.push_back(std::move(path));
      }
      child.close();
      if (static_cast<int>(pendingBooks.size()) >= batchSize) break;
    }
    directory.close();
    yield();
  }
  directories.clear();
  currentBook = 0;
  if (pendingBooks.empty()) {
    state = State::Complete;
  } else {
    state = State::Indexing;
  }
  requestUpdate(true);
}

void Ao3IndexActivity::indexNextBook() {
  if (currentBook >= pendingBooks.size()) {
    state = State::BatchComplete;
    requestUpdate(true);
    return;
  }
  if (!refreshExisting && indexedHashes.size() + indexedCount >= MAX_LIBRARY_BOOKS) {
    state = State::Complete;
    requestUpdate(true);
    return;
  }
  if (ESP.getFreeHeap() < 80U * 1024U || ESP.getMaxAllocHeap() < 48U * 1024U) {
    yield();
    return;
  }

  const std::string path = pendingBooks[currentBook];
  currentTitle = path.substr(path.find_last_of('/') + 1);
  const uint32_t hash = pathHash(path);
  const auto insertAttempted = [this, hash] {
    const auto it = std::lower_bound(attemptedHashes.begin(), attemptedHashes.end(), hash);
    if (it == attemptedHashes.end() || *it != hash) attemptedHashes.insert(it, hash);
  };

  Epub epub(path, "/.crosspoint");
  const bool loaded = epub.load(true, true, Epub::XLocationLoadMode::Skip);
  bool ao3 = loaded && (epub.hasAo3Info() || Ao3Librarian::sniffNativeAo3Preface(epub));
  if (loaded && !ao3) ao3 = isAo3Publisher(epub.sniffPublisher());
  if (ao3 && Ao3Librarian::scrape(epub, true)) {
    ++indexedCount;
  } else {
    ++failedCount;
  }
  insertAttempted();
  ++currentBook;
  requestUpdate(true);
}

void Ao3IndexActivity::loop() {
  if ((state == State::Discovering || state == State::Indexing) &&
      mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    directories.clear();
    pendingBooks.clear();
    finish();
    return;
  }
  if (state == State::Discovering) {
    if (refreshExisting) {
      discoverRefreshCandidates();
      requestUpdate(true);
    } else {
      discoverNextDirectory();
    }
    return;
  }
  if (state == State::Indexing) {
    indexNextBook();
    return;
  }
  if (state == State::Confirm) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) collectNextBatch();
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
    return;
  }
  if (state == State::BatchComplete) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) collectNextBatch();
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
    return;
  }
  if (state == State::Complete || state == State::Error) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
    }
  }
}

void Ao3IndexActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, "AO3 Auto Index", false);
  } else {
    GUI.drawHeader(renderer, header, "AO3 Auto Index");
  }
  const int center = renderer.getScreenHeight() / 2;
  char primary[96] = {};
  char secondary[128] = {};
  switch (state) {
    case State::Discovering:
      snprintf(primary, sizeof(primary), "Scanning %.70s", scanRoot.c_str());
      snprintf(secondary, sizeof(secondary), "%u %s EPUB%s found", static_cast<unsigned>(unindexedCount),
               refreshExisting ? "AO3" : "new",
               unindexedCount == 1 ? "" : "s");
      break;
    case State::Confirm:
      snprintf(primary, sizeof(primary), "%u %s EPUB%s found", static_cast<unsigned>(unindexedCount),
               refreshExisting ? "AO3" : "new",
               unindexedCount == 1 ? "" : "s");
      snprintf(secondary, sizeof(secondary), "%s the next batch of %d?",
               refreshExisting ? "Refresh" : "Index", batchSize);
      break;
    case State::Indexing:
      snprintf(primary, sizeof(primary), "Indexing %u / %u", static_cast<unsigned>(currentBook + 1),
               static_cast<unsigned>(pendingBooks.size()));
      snprintf(secondary, sizeof(secondary), "%.70s", currentTitle.c_str());
      break;
    case State::BatchComplete:
      snprintf(primary, sizeof(primary), "Batch complete");
      snprintf(secondary, sizeof(secondary), "%u indexed, %u skipped - continue?",
               static_cast<unsigned>(indexedCount), static_cast<unsigned>(failedCount));
      break;
    case State::Complete:
      snprintf(primary, sizeof(primary), "AO3 indexing complete");
      snprintf(secondary, sizeof(secondary), "%u indexed, %u skipped",
               static_cast<unsigned>(indexedCount), static_cast<unsigned>(failedCount));
      break;
    case State::Error:
      snprintf(primary, sizeof(primary), "Unable to index");
      snprintf(secondary, sizeof(secondary), "%.110s", errorMessage.c_str());
      break;
  }
  renderer.drawCenteredText(UI_12_FONT_ID, center - 30, primary, true, EpdFontFamily::BOLD);
  const auto lines = renderer.wrappedText(UI_10_FONT_ID, secondary, renderer.getScreenWidth() - 60, 3);
  int y = center + 10;
  for (const auto& line : lines) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, line.c_str());
    y += renderer.getLineHeight(UI_10_FONT_ID);
  }
  const bool choice = state == State::Confirm || state == State::BatchComplete;
  const bool done = state == State::Complete || state == State::Error;
  const auto labels = choice ? mappedInput.mapLabels("Stop", "Continue", "", "")
                             : (done ? mappedInput.mapLabels("", "Done", "", "")
                                     : mappedInput.mapLabels("Cancel", "", "", ""));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
