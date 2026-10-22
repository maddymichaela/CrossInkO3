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
#include <memory>
#include <utility>

#include "Ao3Librarian.h"
#include "Ao3ReadingState.h"
#include "Ao3SyncMemoryPolicy.h"
#include "Ao3SyncUrls.h"
#include "Ao3UpdateParser.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/RenderLock.h"
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
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 "
    "Safari/537.36";
constexpr char AO3_HTML_ACCEPT[] =
    "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8";
constexpr char AO3_EPUB_ACCEPT[] = "application/epub+zip,application/octet-stream;q=0.9,*/*;q=0.8";
constexpr char AO3_ACCEPT_LANGUAGE[] = "en-US,en;q=0.5";

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

void configureAo3Request(HttpDownloader::DownloadOptions& options, const char* accept) {
  options.userAgent = AO3_USER_AGENT;
  options.headers.emplace_back("Accept", accept);
  options.headers.emplace_back("Accept-Language", AO3_ACCEPT_LANGUAGE);
  options.headers.emplace_back("Connection", "keep-alive");
}

bool writeFallbackLibraryInfo(const std::string& cachePath, Ao3LibraryMetadata& metadata,
                              const std::string& bookPath, const std::string& updatedDate,
                              const bool completed) {
  copyField(metadata.filepath, sizeof(metadata.filepath), bookPath);
  copyField(metadata.updatedDate, sizeof(metadata.updatedDate), updatedDate);
  metadata.isCompleted = completed ? 1 : 0;

  Storage.mkdir(cachePath.c_str());
  const std::string infoPath = cachePath + "/ao3_library_info";
  const std::string tempPath = infoPath + ".tmp";
  if (Storage.exists(tempPath.c_str()) && !Storage.remove(tempPath.c_str())) return false;

  FsFile file;
  if (!Storage.openFileForWrite("AO3U", tempPath, file)) return false;
  const bool written = file.write(reinterpret_cast<const uint8_t*>(&metadata), sizeof(metadata)) == sizeof(metadata);
  const bool synced = written && file.sync();
  const bool closed = file.close();
  if (!written || !synced || !closed) {
    Storage.remove(tempPath.c_str());
    return false;
  }

  if (Storage.exists(infoPath.c_str()) && !Storage.remove(infoPath.c_str())) {
    Storage.remove(tempPath.c_str());
    return false;
  }
  if (!Storage.rename(tempPath.c_str(), infoPath.c_str())) {
    Storage.remove(tempPath.c_str());
    return false;
  }
  return true;
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
  return state_ == State::Connecting || state_ == State::Checking || state_ == State::CheckingEpub ||
         state_ == State::Downloading;
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
  updateAlreadyDownloaded_ = false;
  Storage.remove((bookPath_ + TEMP_SUFFIX).c_str());
  requestUpdateAndWait();

  if (!ensureNetworkHeadroom("update check")) {
    requestUpdate();
    return;
  }

  const char* hosts[] = {"archiveofourown.org", "archiveofourown.gay"};
  const HttpDownloader::Transport transports[] = {HttpDownloader::Transport::ESP_HTTP,
                                                   HttpDownloader::Transport::WOLFSSL};
  HttpDownloader::DownloadError lastResult = HttpDownloader::HTTP_ERROR;
  int lastHttpStatus = 0;
  bool sawDate = false;
  bool sawChapters = false;
  for (const char* host : hosts) {
    for (const auto transport : transports) {
      Ao3UpdateParser parser;
      int attemptStatus = 0;
      HttpDownloader::DownloadOptions options;
      options.shouldCancel = [this]() { return cancellationRequested(); };
      options.transport = transport;
      options.responseStatus = &attemptStatus;
      options.acceptEarlyStop = true;
      configureAo3Request(options, AO3_HTML_ACCEPT);
      lastResult = HttpDownloader::streamUrl(
          Ao3SyncUrls::workPage(host, workId_),
          [&parser](const uint8_t* data, const size_t length) { return parser.feed(data, length); }, nullptr, "", "",
          std::move(options));

      if (attemptStatus > 0) lastHttpStatus = attemptStatus;
      sawDate = sawDate || parser.result().hasDate;
      sawChapters = sawChapters || parser.result().hasChapterStatus;
      if (lastResult == HttpDownloader::ABORTED || cancelRequested_) {
        returnToReader();
        return;
      }
      if (lastResult == HttpDownloader::OK && parser.complete()) {
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

      // A real HTTP response will not improve by repeating it with another TLS
      // implementation. Use wolfSSL only when verified TLS failed pre-response.
      if (attemptStatus > 0) break;
    }
  }

  if (lastHttpStatus == 403) {
    state_ = State::CheckingEpub;
    downloadProgress_ = 0;
    downloadTotal_ = 0;
    requestUpdateAndWait();

    const std::string tempPath = bookPath_ + TEMP_SUFFIX;
    int directStatus = 0;
    int fallbackStatus = 0;
    if (fetchEpub(tempPath, "", directStatus, fallbackStatus)) {
      std::string epubDate;
      bool epubCompleted = false;
      if (inspectDownloadedEpub(tempPath, epubDate, epubCompleted)) {
        remoteDate_ = std::move(epubDate);
        remoteCompleted_ = epubCompleted;
        if (localDate_.empty() || remoteDate_ > localDate_) {
          updateAlreadyDownloaded_ = true;
          state_ = State::UpdateAvailable;
          Ao3ReadingStateStore::save(Epub::cachePathForFilePath(bookPath_, "/.crosspoint"),
                                     Ao3ReadingState::UpdateAvailable);
        } else {
          Storage.remove(tempPath.c_str());
          state_ = State::UpToDate;
        }
        requestUpdate();
        return;
      }
      Storage.remove(tempPath.c_str());
      setError("AO3 EPUB did not contain matching work metadata.");
    } else if (cancelRequested_) {
      Storage.remove(tempPath.c_str());
      returnToReader();
      return;
    } else if (directStatus == 403) {
      setError("Direct AO3 EPUB was refused (403). The work may be locked.");
    } else if (directStatus > 0) {
      setError("Direct AO3 EPUB failed (HTTP " + std::to_string(directStatus) + ").");
    } else if (fallbackStatus > 0) {
      setError("Direct EPUB connection failed; fallback HTTP " + std::to_string(fallbackStatus) + ".");
    } else {
      setError("Could not connect to the direct AO3 EPUB host.");
    }
  } else if (lastHttpStatus == 429) {
    setError("AO3 rate limit reached. Please try again later.");
  } else if (lastHttpStatus == 404) {
    setError("AO3 work was not found (404).");
  } else if (lastHttpStatus > 0 && lastHttpStatus != 200) {
    setError("AO3 request failed (HTTP " + std::to_string(lastHttpStatus) + "). Press Select to retry.");
  } else if (lastHttpStatus == 200 || sawDate || sawChapters) {
    setError(std::string("AO3 page missing ") + (!sawDate ? "date" : "chapter") + " metadata.");
  } else if (lastResult == HttpDownloader::FILE_ERROR) {
    setError("AO3 response ended before metadata. Press Select to retry.");
  } else {
    setError("Could not connect to AO3. Press Select to retry.");
  }
  requestUpdate();
}

bool AO3SyncActivity::fetchEpub(const std::string& path, const std::string& date, int& directStatus,
                                int& fallbackStatus) {
  // Contact AO3's file host directly first. The front domains may reject the
  // embedded client before returning their redirect to this host.
  const char* hosts[] = {"download.archiveofourown.org", "archiveofourown.org", "archiveofourown.gay"};
  const HttpDownloader::Transport transports[] = {HttpDownloader::Transport::ARDUINO_HTTP,
                                                   HttpDownloader::Transport::ESP_HTTP,
                                                   HttpDownloader::Transport::WOLFSSL};
  HttpDownloader::DownloadError result = HttpDownloader::HTTP_ERROR;
  directStatus = 0;
  fallbackStatus = 0;
  for (size_t hostIndex = 0; hostIndex < sizeof(hosts) / sizeof(hosts[0]); ++hostIndex) {
    const char* host = hosts[hostIndex];
    for (const auto transport : transports) {
      int attemptStatus = 0;
      HttpDownloader::DownloadOptions options;
      options.shouldCancel = [this]() { return cancellationRequested(); };
      options.transport = transport;
      options.responseStatus = &attemptStatus;
      configureAo3Request(options, AO3_EPUB_ACCEPT);
      result = HttpDownloader::downloadToFile(
          Ao3SyncUrls::download(host, workId_, date), path,
          [this](const size_t downloaded, const size_t total) {
            downloadProgress_ = downloaded;
            downloadTotal_ = total;
            requestUpdate(true);
          },
          &cancelRequested_, "", "", std::move(options));
      if (attemptStatus > 0) {
        if (hostIndex == 0) {
          directStatus = attemptStatus;
        } else {
          fallbackStatus = attemptStatus;
        }
      }
      if (result == HttpDownloader::OK || result == HttpDownloader::ABORTED) break;
      Storage.remove(path.c_str());
      if (attemptStatus > 0) break;
    }
    if (result == HttpDownloader::OK || result == HttpDownloader::ABORTED) break;
  }
  return result == HttpDownloader::OK;
}

bool AO3SyncActivity::inspectDownloadedEpub(const std::string& path, std::string& updatedDate, bool& completed) {
  ZipFile zip(path);
  if (!zip.open()) return false;
  zip.close();

  bool valid = false;
  {
    RenderLock renderLock(*this);
    Epub downloaded(path, VALIDATION_CACHE_ROOT);
    GfxRenderer::FrameBufferLoan loan(renderer);
    const bool loaded = downloaded.load(true, true, Epub::XLocationLoadMode::Skip);
    if (loaded) {
      std::string downloadedWorkId = downloaded.getAo3WorkId();
      updatedDate = downloaded.getAo3UpdateDate();
      completed = downloaded.isAo3Completed();
      if (downloadedWorkId.empty() || updatedDate.empty()) {
        Ao3WorkIdentity identity;
        if (Ao3Librarian::readWorkIdentity(downloaded, identity)) {
          downloadedWorkId = std::move(identity.workId);
          updatedDate = std::move(identity.updatedDate);
          completed = identity.isCompleted;
        }
      }
      valid = downloadedWorkId == workId_ && !updatedDate.empty();
    }
    loan.end();
  }
  Epub(path, VALIDATION_CACHE_ROOT).clearCache();
  return valid;
}

bool AO3SyncActivity::restoreOriginalEpub(const std::string& backupPath) {
  if (Storage.exists(bookPath_.c_str()) && !Storage.remove(bookPath_.c_str())) {
    LOG_ERR("AO3U", "Failed to remove rejected EPUB before rollback: %s", bookPath_.c_str());
    return false;
  }
  if (!Storage.rename(backupPath.c_str(), bookPath_.c_str())) {
    LOG_ERR("AO3U", "Failed to restore EPUB backup: %s", backupPath.c_str());
    return false;
  }
  return true;
}

bool AO3SyncActivity::installDownloadedEpub(const std::string& tempPath, std::string& failureMessage) {
  failureMessage.clear();
  const std::string backupPath = bookPath_ + BACKUP_SUFFIX;
  Epub cacheLocator(bookPath_, "/.crosspoint");
  const std::string cachePath = cacheLocator.getCachePath();
  auto previousMetadata = std::unique_ptr<Ao3LibraryMetadata>(new Ao3LibraryMetadata());
  const bool hadPreviousMetadata = Ao3Librarian::getLibraryInfo(bookPath_, *previousMetadata);

  if (Storage.exists(backupPath.c_str()) && !Storage.remove(backupPath.c_str())) {
    failureMessage = "Could not remove the previous AO3 backup";
    LOG_ERR("AO3U", "%s: %s", failureMessage.c_str(), backupPath.c_str());
    return false;
  }
  if (!Storage.rename(bookPath_.c_str(), backupPath.c_str())) {
    failureMessage = "Could not back up the current EPUB";
    LOG_ERR("AO3U", "%s: %s", failureMessage.c_str(), bookPath_.c_str());
    return false;
  }
  if (!Storage.rename(tempPath.c_str(), bookPath_.c_str())) {
    const bool restored = Storage.rename(backupPath.c_str(), bookPath_.c_str());
    failureMessage = restored ? "Could not place the downloaded EPUB"
                              : "Could not restore the original EPUB; keep its .ao3bak file";
    LOG_ERR("AO3U", "%s: %s", failureMessage.c_str(), bookPath_.c_str());
    return false;
  }

  if (!clearBookCachePreservingUserState(bookPath_)) {
    const bool restored = restoreOriginalEpub(backupPath);
    if (hadPreviousMetadata &&
        !writeFallbackLibraryInfo(cachePath, *previousMetadata, bookPath_, localDate_,
                                  previousMetadata->isCompleted != 0)) {
      LOG_ERR("AO3U", "Could not restore AO3 library metadata after cache rollback");
    }
    failureMessage = restored ? "Could not preserve reader data for the AO3 update"
                              : "Could not restore the original EPUB; keep its .ao3bak file";
    LOG_ERR("AO3U", "%s: %s", failureMessage.c_str(), cachePath.c_str());
    return false;
  }

  if (hadPreviousMetadata &&
      !writeFallbackLibraryInfo(cachePath, *previousMetadata, bookPath_, remoteDate_, remoteCompleted_)) {
    LOG_ERR("AO3U", "Could not retain the previous AO3 library metadata; a fresh scrape will be attempted");
  }
  previousMetadata.reset();

  Storage.mkdir(cachePath.c_str());
  Epub updated(bookPath_, "/.crosspoint");
  updated.saveAo3Info(workId_, remoteDate_, remoteCompleted_);

  Ao3ReadingStateStore::remove(cachePath);
  BookReadingStats stats = BookReadingStats::load(cachePath);
  stats.isCompleted = false;
  if (!stats.finishedDateManual) stats.finishedDate = {};
  stats.save(cachePath);

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    WiFi.mode(WIFI_OFF);
  }

  bool metadataRefreshed = false;
  {
    RenderLock renderLock(*this);
    GfxRenderer::FrameBufferLoan loan(renderer);
    const bool loaded = updated.load(true, true, Epub::XLocationLoadMode::Skip);
    loan.end();
    if (loaded) {
      metadataRefreshed = Ao3Librarian::scrape(updated, true);
    } else {
      LOG_ERR("AO3U", "Installed EPUB cache will be rebuilt when the reader opens the book");
    }
  }

  if (!metadataRefreshed) {
    LOG_ERR("AO3U", "Installed EPUB is valid, but rich AO3 metadata refresh was deferred");
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

  if (!updateAlreadyDownloaded_ && !ensureNetworkHeadroom("EPUB download", true)) {
    requestUpdate();
    return;
  }

  const std::string tempPath = bookPath_ + TEMP_SUFFIX;
  int directStatus = 0;
  int fallbackStatus = 0;
  const bool downloaded = updateAlreadyDownloaded_ && Storage.exists(tempPath.c_str());
  if (!downloaded) Storage.remove(tempPath.c_str());
  const bool fetched = downloaded || fetchEpub(tempPath, remoteDate_, directStatus, fallbackStatus);
  if (!fetched && cancelRequested_) {
    Storage.remove(tempPath.c_str());
    state_ = State::UpdateAvailable;
    requestUpdate();
    return;
  }
  if (!fetched) {
    Storage.remove(tempPath.c_str());
    setError(directStatus > 0 ? "AO3 EPUB download failed (HTTP " + std::to_string(directStatus) + ")"
                              : "AO3 EPUB download connection failed",
             true);
    requestUpdate();
    return;
  }
  std::string downloadedDate;
  bool downloadedCompleted = false;
  if (!inspectDownloadedEpub(tempPath, downloadedDate, downloadedCompleted)) {
    Storage.remove(tempPath.c_str());
    setError("Downloaded EPUB did not match this AO3 work", true);
    requestUpdate();
    return;
  }
  remoteDate_ = std::move(downloadedDate);
  remoteCompleted_ = downloadedCompleted;
  std::string installFailure;
  if (!installDownloadedEpub(tempPath, installFailure)) {
    Storage.remove(tempPath.c_str());
    setError(installFailure.empty() ? "Could not safely install the AO3 update" : std::move(installFailure), true);
    requestUpdate();
    return;
  }

  retryDownload_ = false;
  updateAlreadyDownloaded_ = false;
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
  Storage.remove((bookPath_ + TEMP_SUFFIX).c_str());
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
    case State::CheckingEpub:
      renderer.drawCenteredText(UI_10_FONT_ID, centerY - 30, "AO3 page blocked; checking EPUB...");
      if (downloadTotal_ > 0) {
        GUI.drawProgressBar(renderer,
                            Rect{metrics.contentSidePadding, centerY + 10,
                                 renderer.getScreenWidth() - metrics.contentSidePadding * 2, metrics.progressBarHeight},
                            static_cast<int>((downloadProgress_ * 100) / downloadTotal_), 100);
      }
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
