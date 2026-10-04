#include "AO3SyncActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <ZipFile.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "Ao3Librarian.h"
#include "Ao3ReadingState.h"
#include "Ao3UpdateParser.h"
#include "Ao3SyncMemoryPolicy.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/BookReadingStats.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"

namespace {
constexpr char REQUEST_PATH[] = "/.crosspoint/ao3_update_request.bin";
constexpr char REQUEST_MAGIC[] = "AO3U";
constexpr uint8_t REQUEST_VERSION = 1;
constexpr char TEMP_SUFFIX[] = ".ao3tmp";
constexpr char BACKUP_SUFFIX[] = ".ao3bak";
constexpr char VALIDATION_CACHE_ROOT[] = "/.crosspoint/ao3_validation";
constexpr char AO3_USER_AGENT[] =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120 Safari/537.36";

struct Ao3UpdateRequestRecord {
  char magic[4] = {'A', 'O', '3', 'U'};
  uint8_t version = REQUEST_VERSION;
  char bookPath[256] = {};
  char workId[24] = {};
  char localDate[12] = {};
};

void copyField(char* destination, const size_t capacity, const std::string& source) {
  if (!destination || capacity == 0) return;
  const size_t count = std::min(capacity - 1, source.size());
  memcpy(destination, source.data(), count);
  destination[count] = '\0';
}

bool hasActiveWifiConnection() { return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0); }

std::string workPageUrl(const char* host, const std::string& workId) {
  return std::string("https://") + host + "/works/" + workId + "?view_adult=true";
}

std::string downloadUrl(const char* host, const std::string& workId, const std::string& date) {
  return std::string("https://") + host + "/downloads/" + workId + "/work.epub?v=" + date;
}
}  // namespace

AO3SyncActivity::AO3SyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                                 std::string workId, std::string localDate, const bool networkBootReady)
    : Activity("AO3Sync", renderer, mappedInput),
      bookPath_(std::move(bookPath)),
      workId_(std::move(workId)),
      localDate_(std::move(localDate)),
      networkBootReady_(networkBootReady) {}

bool AO3SyncActivity::saveRequest() const {
  if (bookPath_.empty() || workId_.empty()) return false;
  Storage.mkdir("/.crosspoint");
  FsFile file;
  if (!Storage.openFileForWrite("AO3U", REQUEST_PATH, file)) return false;
  Ao3UpdateRequestRecord record;
  copyField(record.bookPath, sizeof(record.bookPath), bookPath_);
  copyField(record.workId, sizeof(record.workId), workId_);
  copyField(record.localDate, sizeof(record.localDate), localDate_);
  const bool ok = file.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record)) == sizeof(record) && file.sync();
  file.close();
  return ok;
}

bool AO3SyncActivity::loadRequest() {
  FsFile file;
  if (!Storage.openFileForRead("AO3U", REQUEST_PATH, file)) return false;
  Ao3UpdateRequestRecord record;
  const bool ok = file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) == sizeof(record);
  file.close();
  if (!ok || memcmp(record.magic, REQUEST_MAGIC, sizeof(record.magic)) != 0 || record.version != REQUEST_VERSION ||
      record.bookPath[0] == '\0' || record.workId[0] == '\0') {
    return false;
  }
  record.bookPath[sizeof(record.bookPath) - 1] = '\0';
  record.workId[sizeof(record.workId) - 1] = '\0';
  record.localDate[sizeof(record.localDate) - 1] = '\0';
  bookPath_ = record.bookPath;
  workId_ = record.workId;
  localDate_ = record.localDate;
  return true;
}

void AO3SyncActivity::removeRequest() const { Storage.remove(REQUEST_PATH); }

void AO3SyncActivity::onEnter() {
  Activity::onEnter();
  if (!networkBootReady_) {
    if (!saveRequest()) {
      setError("Could not save AO3 update request");
      requestUpdate();
      return;
    }
    silentRestartToNetwork(NetworkBootTarget::AO3_UPDATE);
    return;
  }

  if (!loadRequest() || !Storage.exists(bookPath_.c_str())) {
    setError("AO3 update request is invalid");
    requestUpdate();
    return;
  }

  sdFontSystem.releaseForNetwork(renderer);
  if (hasActiveWifiConnection()) {
    onWifiSelectionComplete(true);
    return;
  }

  state_ = State::Connecting;
  WiFi.mode(WIFI_STA);
  requestUpdate();
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void AO3SyncActivity::onExit() {
  Activity::onExit();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    WiFi.mode(WIFI_OFF);
  }
}

bool AO3SyncActivity::preventAutoSleep() {
  return state_ == State::Connecting || state_ == State::Checking || state_ == State::Downloading;
}

void AO3SyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    returnToReader();
    return;
  }
  checkForUpdate();
}

bool AO3SyncActivity::cancellationRequested() {
  if (cancelRequested_) return true;
  mappedInput.update();
  if (mappedInput.wasHomeGesture() || mappedInput.isPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancelRequested_ = true;
  }
  return cancelRequested_;
}

void AO3SyncActivity::checkForUpdate() {
  state_ = State::Checking;
  errorMessage_.clear();
  remoteDate_.clear();
  remoteCompleted_ = false;
  cancelRequested_ = false;
  retryDownload_ = false;
  requestUpdateAndWait();

  if (!ensureNetworkHeadroom("update check")) {
    requestUpdate();
    return;
  }

  const char* hosts[] = {"archiveofourown.org", "archiveofourown.gay"};
  for (const char* host : hosts) {
    Ao3UpdateParser parser;
    HttpDownloader::DownloadOptions options;
    options.shouldCancel = [this]() { return cancellationRequested(); };
    options.userAgent = AO3_USER_AGENT;
    const auto result = HttpDownloader::streamUrl(
        workPageUrl(host, workId_),
        [&parser](const uint8_t* data, const size_t length) {
          parser.feed(data, length);
          return true;
        },
        nullptr, "", "", options);

    if (result == HttpDownloader::ABORTED || cancelRequested_) {
      returnToReader();
      return;
    }
    if (result != HttpDownloader::OK || !parser.complete()) {
      continue;
    }

    remoteDate_ = parser.result().updatedDate;
    remoteCompleted_ = parser.result().isCompleted;
    if (localDate_.empty() || remoteDate_ > localDate_) {
      state_ = State::UpdateAvailable;
      Ao3ReadingStateStore::save(Epub::cachePathForFilePath(bookPath_, "/.crosspoint"),
                                 Ao3ReadingState::UpdateAvailable);
    } else {
      state_ = State::UpToDate;
    }
    requestUpdate();
    return;
  }

  setError("Could not read AO3 work information");
  requestUpdate();
}

bool AO3SyncActivity::validateDownloadedEpub(const std::string& path) const {
  ZipFile zip(path);
  if (!zip.open()) return false;
  zip.close();

  bool valid = false;
  {
    Epub downloaded(path, VALIDATION_CACHE_ROOT);
    GfxRenderer::FrameBufferLoan loan(renderer);
    const bool loaded = downloaded.load(true, true, Epub::XLocationLoadMode::Skip);
    loan.end();
    const std::string downloadedWorkId = loaded ? downloaded.getAo3WorkId() : std::string{};
    valid = loaded && downloaded.isAo3Work() && downloadedWorkId == workId_;
  }
  Epub(path, VALIDATION_CACHE_ROOT).clearCache();
  return valid;
}

void AO3SyncActivity::restoreOriginalEpub(const std::string& backupPath) const {
  if (Storage.exists(bookPath_.c_str())) Storage.remove(bookPath_.c_str());
  if (!Storage.rename(backupPath.c_str(), bookPath_.c_str())) {
    LOG_ERR("AO3U", "Failed to restore EPUB backup: %s", backupPath.c_str());
    return;
  }

  clearBookCachePreservingUserState(bookPath_);
  Epub original(bookPath_, "/.crosspoint");
  GfxRenderer::FrameBufferLoan loan(renderer);
  const bool loaded = original.load(true, true, Epub::XLocationLoadMode::Skip);
  loan.end();
  if (loaded) Ao3Librarian::scrape(original);
}

bool AO3SyncActivity::installDownloadedEpub(const std::string& tempPath) {
  const std::string backupPath = bookPath_ + BACKUP_SUFFIX;
  if (Storage.exists(backupPath.c_str()) && !Storage.remove(backupPath.c_str())) return false;
  if (!Storage.rename(bookPath_.c_str(), backupPath.c_str())) return false;
  if (!Storage.rename(tempPath.c_str(), bookPath_.c_str())) {
    Storage.rename(backupPath.c_str(), bookPath_.c_str());
    return false;
  }

  if (!clearBookCachePreservingUserState(bookPath_)) {
    restoreOriginalEpub(backupPath);
    return false;
  }

  bool accepted = false;
  {
    Epub updated(bookPath_, "/.crosspoint");
    GfxRenderer::FrameBufferLoan loan(renderer);
    const bool loaded = updated.load(true, true, Epub::XLocationLoadMode::Skip);
    loan.end();
    if (loaded && updated.getAo3WorkId() == workId_) {
      updated.saveAo3Info(workId_, remoteDate_, remoteCompleted_);
      accepted = Ao3Librarian::scrape(updated);
      if (accepted) {
        const std::string cachePath = updated.getCachePath();
        Ao3ReadingStateStore::remove(cachePath);
        BookReadingStats stats = BookReadingStats::load(cachePath);
        stats.isCompleted = false;
        if (!stats.finishedDateManual) stats.finishedDate = {};
        stats.save(cachePath);
      }
    }
  }

  if (!accepted) {
    restoreOriginalEpub(backupPath);
    return false;
  }

  if (!Storage.remove(backupPath.c_str())) {
    LOG_ERR("AO3U", "Updated EPUB installed but backup could not be removed: %s", backupPath.c_str());
  }
  localDate_ = remoteDate_;
  return true;
}

void AO3SyncActivity::downloadUpdate() {
  state_ = State::Downloading;
  errorMessage_.clear();
  cancelRequested_ = false;
  retryDownload_ = true;
  downloadProgress_ = 0;
  downloadTotal_ = 0;
  requestUpdateAndWait();

  if (!ensureNetworkHeadroom("EPUB download", true)) {
    requestUpdate();
    return;
  }

  const std::string tempPath = bookPath_ + TEMP_SUFFIX;
  Storage.remove(tempPath.c_str());
  const char* hosts[] = {"archiveofourown.org", "archiveofourown.gay"};
  HttpDownloader::DownloadError result = HttpDownloader::HTTP_ERROR;
  for (const char* host : hosts) {
    HttpDownloader::DownloadOptions options;
    options.shouldCancel = [this]() { return cancellationRequested(); };
    options.userAgent = AO3_USER_AGENT;
    result = HttpDownloader::downloadToFile(
        downloadUrl(host, workId_, remoteDate_), tempPath,
        [this](const size_t downloaded, const size_t total) {
          downloadProgress_ = downloaded;
          downloadTotal_ = total;
          requestUpdate(true);
        },
        &cancelRequested_, "", "", options);
    if (result == HttpDownloader::OK || result == HttpDownloader::ABORTED) break;
    Storage.remove(tempPath.c_str());
  }

  if (result == HttpDownloader::ABORTED || cancelRequested_) {
    Storage.remove(tempPath.c_str());
    state_ = State::UpdateAvailable;
    requestUpdate();
    return;
  }
  if (result != HttpDownloader::OK) {
    Storage.remove(tempPath.c_str());
    setError("AO3 EPUB download failed", true);
    requestUpdate();
    return;
  }
  if (!validateDownloadedEpub(tempPath)) {
    Storage.remove(tempPath.c_str());
    setError("Downloaded EPUB did not match this AO3 work", true);
    requestUpdate();
    return;
  }
  if (!installDownloadedEpub(tempPath)) {
    Storage.remove(tempPath.c_str());
    setError("Could not safely install the AO3 update", true);
    requestUpdate();
    return;
  }

  retryDownload_ = false;
  state_ = State::Complete;
  requestUpdate();
}

void AO3SyncActivity::setError(std::string message, const bool downloadRetry) {
  errorMessage_ = std::move(message);
  retryDownload_ = downloadRetry;
  state_ = State::Error;
}

bool AO3SyncActivity::ensureNetworkHeadroom(const char* operation, const bool downloadRetry) {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
  if (Ao3SyncMemoryPolicy::hasNetworkHeadroom(freeHeap, maxAllocHeap)) return true;

  LOG_ERR("AO3U", "Low heap before %s: %u free, %u max alloc (need %u/%u)", operation, freeHeap, maxAllocHeap,
          Ao3SyncMemoryPolicy::MIN_FREE_HEAP, Ao3SyncMemoryPolicy::MIN_MAX_ALLOC_HEAP);
  setError("Not enough memory for AO3 update. Press Back and try again.", downloadRetry);
  return false;
}

void AO3SyncActivity::returnToReader() {
  removeRequest();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    WiFi.mode(WIFI_OFF);
  }
  silentRestartToReader();
}

void AO3SyncActivity::loop() {
  if (state_ == State::UpdateAvailable) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      downloadUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
    return;
  }

  if (state_ == State::UpToDate || state_ == State::Complete) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
    return;
  }

  if (state_ == State::Error) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (retryDownload_ && !remoteDate_.empty()) {
        downloadUpdate();
      } else if (hasActiveWifiConnection()) {
        checkForUpdate();
      } else {
        state_ = State::Connecting;
        WiFi.mode(WIFI_STA);
        requestUpdate();
        startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                               [this](const ActivityResult& result) {
                                 onWifiSelectionComplete(!result.isCancelled);
                               });
      }
    }
  }
}

void AO3SyncActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect header{0, metrics.topPadding, renderer.getScreenWidth(),
                    TouchHeaderBackButton::height(metrics, mappedInput)};
  GUI.drawHeader(renderer, header, "AO3 Update");
  const int centerY = renderer.getScreenHeight() / 2;

  switch (state_) {
    case State::Starting:
    case State::Connecting:
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, "Connecting to Wi-Fi...");
      break;
    case State::Checking:
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, "Checking AO3 for updates...");
      break;
    case State::UpdateAvailable:
      renderer.drawCenteredText(UI_10_FONT_ID, centerY - 20, "New chapter available", true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(SMALL_FONT_ID, centerY + 14, remoteDate_.c_str());
      GUI.drawButtonHints(renderer, tr(STR_CANCEL), tr(STR_UPDATE), "", "");
      break;
    case State::Downloading: {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY - 30, "Downloading AO3 EPUB...");
      if (downloadTotal_ > 0) {
        GUI.drawProgressBar(renderer,
                            Rect{metrics.contentSidePadding, centerY + 10,
                                 renderer.getScreenWidth() - metrics.contentSidePadding * 2, metrics.progressBarHeight},
                            static_cast<int>((downloadProgress_ * 100) / downloadTotal_), 100);
      }
      break;
    }
    case State::UpToDate:
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, "This AO3 work is up to date", true, EpdFontFamily::BOLD);
      GUI.drawButtonHints(renderer, tr(STR_BACK), tr(STR_OK_BUTTON), "", "");
      break;
    case State::Complete:
      renderer.drawCenteredText(UI_10_FONT_ID, centerY - 15, "AO3 update installed", true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(SMALL_FONT_ID, centerY + 18, "Progress and reader data were preserved");
      GUI.drawButtonHints(renderer, tr(STR_BACK), tr(STR_OPEN), "", "");
      break;
    case State::Error:
      renderer.drawCenteredText(UI_10_FONT_ID, centerY - 20, "AO3 update failed", true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(SMALL_FONT_ID, centerY + 15, errorMessage_.c_str());
      GUI.drawButtonHints(renderer, tr(STR_BACK), tr(STR_RETRY), "", "");
      break;
  }
  renderer.displayBuffer();
}
