#include "Ao3ReadingState.h"

#include "Ao3Librarian.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

namespace {
constexpr char AO3_STATE_FILE[] = "/ao3_state.bin";
constexpr uint8_t AO3_STATE_VERSION = 1;
constexpr uint32_t AO3_STATE_MAGIC = 0x3353414F;  // "OAS3" LE

struct Ao3StateRecord {
  uint32_t magic = AO3_STATE_MAGIC;
  uint8_t version = AO3_STATE_VERSION;
  uint8_t state = 0;
  uint8_t reserved[2] = {0, 0};
};

bool isValidState(uint8_t state) {
  return state == static_cast<uint8_t>(Ao3ReadingState::None) ||
         state == static_cast<uint8_t>(Ao3ReadingState::WaitingForChapter) ||
         state == static_cast<uint8_t>(Ao3ReadingState::UpdateAvailable) ||
         state == static_cast<uint8_t>(Ao3ReadingState::Unread) ||
         state == static_cast<uint8_t>(Ao3ReadingState::Reading) ||
         state == static_cast<uint8_t>(Ao3ReadingState::Finished);
}

std::string statePath(const std::string& cachePath) { return cachePath + AO3_STATE_FILE; }
}  // namespace

Ao3ReadingState Ao3ReadingStateStore::load(const std::string& cachePath) {
  FsFile file;
  if (!Storage.openFileForRead("AO3S", statePath(cachePath), file)) {
    return Ao3ReadingState::None;
  }

  Ao3StateRecord record;
  const bool ok = serialization::tryReadPod(file, record);
  file.close();
  if (!ok || record.magic != AO3_STATE_MAGIC || record.version != AO3_STATE_VERSION || !isValidState(record.state)) {
    LOG_DBG("AO3S", "Ignoring invalid AO3 reading state: %s", cachePath.c_str());
    return Ao3ReadingState::None;
  }
  return static_cast<Ao3ReadingState>(record.state);
}

bool Ao3ReadingStateStore::save(const std::string& cachePath, const Ao3ReadingState state) {
  Storage.mkdir(cachePath.c_str());
  if (state == Ao3ReadingState::None) {
    return remove(cachePath);
  }

  const Ao3ReadingState oldState = load(cachePath);

  FsFile file;
  if (!Storage.openFileForWrite("AO3S", statePath(cachePath), file)) {
    return false;
  }

  Ao3StateRecord record;
  record.state = static_cast<uint8_t>(state);
  const bool ok = serialization::tryWritePod(file, record) && file.sync();
  file.close();
  if (ok) Ao3Librarian::updateSummaryForStateChange(oldState, state);
  return ok;
}

bool Ao3ReadingStateStore::remove(const std::string& cachePath) {
  const std::string path = statePath(cachePath);
  if (!Storage.exists(path.c_str())) return true;
  const Ao3ReadingState oldState = load(cachePath);
  const bool removed = Storage.remove(path.c_str());
  if (removed) Ao3Librarian::updateSummaryForStateChange(oldState, Ao3ReadingState::None);
  return removed;
}

const char* Ao3ReadingStateStore::labelFor(const Ao3ReadingState state) {
  switch (state) {
    case Ao3ReadingState::Unread:
      return "Unread";
    case Ao3ReadingState::Reading:
      return "Reading";
    case Ao3ReadingState::WaitingForChapter:
      return "Waiting for Chapter";
    case Ao3ReadingState::UpdateAvailable:
      return "New Chapter Available";
    case Ao3ReadingState::Finished:
      return "Finished";
    case Ao3ReadingState::None:
    default:
      return "";
  }
}

std::vector<std::string> ao3ReadingStateOptions() {
  return {"Automatic", "Unread", "Reading", "Waiting for Chapter", "New Chapter Available", "Finished"};
}

uint8_t ao3ReadingStateOptionIndex(const Ao3ReadingState state) {
  switch (state) {
    case Ao3ReadingState::Unread:
      return 1;
    case Ao3ReadingState::Reading:
      return 2;
    case Ao3ReadingState::WaitingForChapter:
      return 3;
    case Ao3ReadingState::UpdateAvailable:
      return 4;
    case Ao3ReadingState::Finished:
      return 5;
    case Ao3ReadingState::None:
    default:
      return 0;
  }
}

Ao3ReadingState ao3ReadingStateForOption(const uint8_t index) {
  switch (index) {
    case 1:
      return Ao3ReadingState::Unread;
    case 2:
      return Ao3ReadingState::Reading;
    case 3:
      return Ao3ReadingState::WaitingForChapter;
    case 4:
      return Ao3ReadingState::UpdateAvailable;
    case 5:
      return Ao3ReadingState::Finished;
    case 0:
    default:
      return Ao3ReadingState::None;
  }
}
