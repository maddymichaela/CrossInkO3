#include "Ao3ArchiveHelper.h"

#include <ArduinoJson.h>
#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ZipFile.h>

#include <cstdio>
#include <cstring>

#include "Ao3Librarian.h"
#include "Ao3LibraryMetadata.h"
#include "BookMoveUtils.h"
#include "CrossPointSettings.h"
#include "ReadFolderPolicy.h"

namespace {
constexpr char AO3_SETTINGS_PATH[] = "/.crosspoint/ao3_settings.json";
constexpr char ARCHIVE_RECORD_DIR[] = "/.crosspoint/ao3_archive_origins";

std::string trimTrailingSlashes(std::string path) {
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  return path;
}

std::string loadAo3Folder() {
  if (!Storage.exists(AO3_SETTINGS_PATH)) return "";
  const String json = Storage.readFile(AO3_SETTINGS_PATH);
  if (json.isEmpty()) return "";
  JsonDocument document;
  if (deserializeJson(document, json)) return "";
  return trimTrailingSlashes(std::string(document["ao3Folder"] | ""));
}

std::string leafName(const std::string& path) {
  const size_t slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string archiveRecordPath(const std::string& archivedPath) {
  const uint64_t hash = ZipFile::fnvHash64(archivedPath.c_str(), archivedPath.size());
  char filename[32];
  snprintf(filename, sizeof(filename), "/%016llx.path", static_cast<unsigned long long>(hash));
  return std::string(ARCHIVE_RECORD_DIR) + filename;
}

bool writeArchiveRecord(const std::string& archivedPath, const std::string& originalPath) {
  Storage.mkdir(ARCHIVE_RECORD_DIR, true);
  return Storage.writeFile(archiveRecordPath(archivedPath).c_str(), String(originalPath.c_str()));
}

std::string readArchiveRecord(const std::string& archivedPath) {
  const std::string recordPath = archiveRecordPath(archivedPath);
  if (!Storage.exists(recordPath.c_str())) return "";
  const String value = Storage.readFile(recordPath.c_str());
  return value.isEmpty() ? "" : std::string(value.c_str());
}

std::string uniqueDestination(const std::string& desiredPath) {
  if (!Storage.exists(desiredPath.c_str())) return desiredPath;
  const size_t slash = desiredPath.rfind('/');
  const std::string directory = slash == std::string::npos ? "" : desiredPath.substr(0, slash + 1);
  const std::string filename = slash == std::string::npos ? desiredPath : desiredPath.substr(slash + 1);
  const size_t dot = filename.rfind('.');
  const std::string base = dot == std::string::npos ? filename : filename.substr(0, dot);
  const std::string extension = dot == std::string::npos ? "" : filename.substr(dot);
  for (unsigned suffix = 2; suffix < 100; ++suffix) {
    const std::string candidate = directory + base + " (" + std::to_string(suffix) + ")" + extension;
    if (!Storage.exists(candidate.c_str())) return candidate;
  }
  return "";
}

std::string metadataTitle(const Ao3LibraryMetadata& metadata, const std::string& path) {
  return metadata.title[0] ? metadata.title : leafName(path);
}
}  // namespace

namespace Ao3ArchiveHelper {

bool isAo3Fic(const std::string& path) {
  Ao3LibraryMetadata metadata;
  return Ao3Librarian::getLibraryInfo(path, metadata);
}

bool isArchived(const std::string& path) { return !readArchiveRecord(path).empty(); }

void forgetOriginalPath(const std::string& archivedPath) {
  Storage.remove(archiveRecordPath(archivedPath).c_str());
}

std::string buildDestinationPath(const std::string& sourcePath) {
  const std::string ao3Folder = loadAo3Folder();
  const std::string archiveName = ao3Folder.empty() || ao3Folder == "/" ? "AO3 Fanfiction" : leafName(ao3Folder);
  std::string relativePath = leafName(sourcePath);
  if (ao3Folder == "/" && sourcePath.size() > 1 && sourcePath.front() == '/') {
    relativePath = sourcePath.substr(1);
  } else if (!ao3Folder.empty() && sourcePath.size() > ao3Folder.size() &&
             sourcePath.compare(0, ao3Folder.size(), ao3Folder) == 0 && sourcePath[ao3Folder.size()] == '/') {
    relativePath = sourcePath.substr(ao3Folder.size() + 1);
  }

  const std::string desired = ReadFolderPolicy::normalizeFolder(SETTINGS.readFolder) + "/" + archiveName + "/" +
                              relativePath;
  const size_t slash = desired.rfind('/');
  if (slash == std::string::npos) return "";
  const std::string destinationDirectory = desired.substr(0, slash);
  if (!Storage.exists(destinationDirectory.c_str()) && !Storage.mkdir(destinationDirectory.c_str(), true)) return "";
  return uniqueDestination(desired);
}

std::string moveToReadFolder(const std::string& sourcePath, const bool keepInRecents) {
  Ao3LibraryMetadata metadata;
  if (!Ao3Librarian::getLibraryInfo(sourcePath, metadata)) return "";
  const std::string destination = buildDestinationPath(sourcePath);
  if (destination.empty() || !writeArchiveRecord(destination, sourcePath)) return "";

  const std::string oldCachePath = Epub::cachePathForFilePath(sourcePath, "/.crosspoint");
  if (!Storage.rename(sourcePath.c_str(), destination.c_str())) {
    Storage.remove(archiveRecordPath(destination).c_str());
    return "";
  }
  if (!BookMoveUtils::migrateMovedEpubState(sourcePath, destination, oldCachePath,
                                             metadataTitle(metadata, sourcePath), metadata.author, keepInRecents)) {
    LOG_ERR("AO3Archive", "Some state failed to migrate for %s -> %s", sourcePath.c_str(), destination.c_str());
  }
  return destination;
}

std::string restoreOriginalFolder(const std::string& archivedPath, const bool keepInRecents) {
  const std::string originalPath = readArchiveRecord(archivedPath);
  if (originalPath.empty() || Storage.exists(originalPath.c_str())) return "";
  const size_t slash = originalPath.rfind('/');
  if (slash == std::string::npos) return "";
  const std::string originalDirectory = originalPath.substr(0, slash);
  if (!Storage.exists(originalDirectory.c_str()) && !Storage.mkdir(originalDirectory.c_str(), true)) return "";

  Ao3LibraryMetadata metadata;
  if (!Ao3Librarian::getLibraryInfo(archivedPath, metadata)) return "";
  const std::string oldCachePath = Epub::cachePathForFilePath(archivedPath, "/.crosspoint");
  if (!Storage.rename(archivedPath.c_str(), originalPath.c_str())) return "";
  if (!BookMoveUtils::migrateMovedEpubState(archivedPath, originalPath, oldCachePath,
                                             metadataTitle(metadata, archivedPath), metadata.author, keepInRecents)) {
    LOG_ERR("AO3Archive", "Some state failed to restore for %s -> %s", archivedPath.c_str(), originalPath.c_str());
  }
  forgetOriginalPath(archivedPath);
  return originalPath;
}

}  // namespace Ao3ArchiveHelper
