#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"

class Ao3IndexActivity final : public Activity {
 public:
  Ao3IndexActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string scanRoot, int batchSize,
                   std::vector<std::string> ignoredFolders = {}, bool refreshExisting = false);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool preventAutoSleep() override { return true; }

 private:
  enum class State : uint8_t { Discovering, Confirm, Indexing, BatchComplete, Complete, Error };

  struct DirectoryEntry {
    std::string path;
    uint8_t depth = 0;
  };

  State state = State::Discovering;
  std::string scanRoot;
  std::vector<std::string> ignoredFolders;
  int batchSize = 10;
  bool refreshExisting = false;
  std::vector<DirectoryEntry> directories;
  std::vector<uint32_t> indexedHashes;
  std::vector<uint32_t> attemptedHashes;
  std::vector<std::string> pendingBooks;
  size_t unindexedCount = 0;
  size_t indexedCount = 0;
  size_t failedCount = 0;
  size_t currentBook = 0;
  std::string currentTitle;
  std::string errorMessage;

  void buildIndexedHashes();
  void discoverNextDirectory();
  void discoverRefreshCandidates();
  void collectNextBatch();
  void indexNextBook();
  bool alreadyHandled(uint32_t hash) const;
  bool isIgnored(const std::string& path) const;
  bool isUnderScanRoot(const std::string& path) const;
  static bool isEpubName(std::string name);
  static uint32_t pathHash(const std::string& path);
};
