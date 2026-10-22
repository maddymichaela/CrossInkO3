#pragma once

#include <cstddef>
#include <string>

#include "activities/Activity.h"

class AO3SyncActivity final : public Activity {
 public:
  AO3SyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath = {},
                  std::string workId = {}, std::string localDate = {}, bool networkBootReady = false);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;

 private:
  enum class State {
    Starting,
    Connecting,
    Checking,
    CheckingEpub,
    UpdateAvailable,
    Downloading,
    UpToDate,
    Complete,
    Error,
  };

  bool saveRequest() const;
  bool loadRequest();
  void removeRequest() const;
  void onWifiSelectionComplete(bool success);
  void checkForUpdate();
  void downloadUpdate();
  bool fetchEpub(const std::string& path, const std::string& date, int& directStatus, int& fallbackStatus);
  bool inspectDownloadedEpub(const std::string& path, std::string& updatedDate, bool& completed);
  bool installDownloadedEpub(const std::string& tempPath);
  void restoreOriginalEpub(const std::string& backupPath);
  void setError(std::string message, bool downloadRetry = false);
  bool ensureNetworkHeadroom(const char* operation, bool downloadRetry = false);
  bool cancellationRequested();
  void returnToReader();

  State state_ = State::Starting;
  std::string bookPath_;
  std::string workId_;
  std::string localDate_;
  std::string remoteDate_;
  std::string errorMessage_;
  bool remoteCompleted_ = false;
  bool networkBootReady_ = false;
  bool cancelRequested_ = false;
  bool retryDownload_ = false;
  bool updateAlreadyDownloaded_ = false;
  size_t downloadProgress_ = 0;
  size_t downloadTotal_ = 0;
};
