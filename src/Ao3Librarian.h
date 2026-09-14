#pragma once

#include <functional>
#include <string>
#include <vector>
#include "Ao3LibraryMetadata.h"
#include "Ao3ReadingState.h"

#include "Ao3CompactIndexRecord.h"

class Epub;

struct Ao3LibrarySummary {
  uint16_t total = 0;
  uint16_t waiting = 0;
  uint16_t updatesAvailable = 0;
};

/**
 * @brief Utility class to scrape AO3 metadata from an EPUB file.
 * Handles FanFicFare-exported AO3 EPUBs and AO3-download (Calibre-style) EPUBs.
 */
class Ao3Librarian {
 public:
  /**
   * @brief True if spine 0 looks like an AO3-download preface (no ao3WorkId in OPF).
   */
  static bool sniffNativeAo3Preface(const Epub& epub);

  /**
   * @brief Scrapes metadata from the given EPUB and saves it to the sidecar.
   * @param epub The EPUB object (must be loaded).
   * @param force If true, overwrites existing library info.
   * @return true if successful or if info already exists.
   */
  static bool scrape(const Epub& epub, bool force = false);

  /**
   * @brief Reads the library info sidecar if it exists.
   * @param epub The EPUB object.
   * @param meta Out parameter for metadata.
   * @return true if metadata was found and is valid.
   */
  static bool getLibraryInfo(const Epub& epub, Ao3LibraryMetadata& meta);

  /**
   * @brief Reads an EPUB's AO3 sidecar without constructing or loading an Epub.
   */
  static bool getLibraryInfo(const std::string& epubPath, Ao3LibraryMetadata& meta);

  /**
   * @brief Streams identified AO3 metadata one sidecar at a time.
   *
   * Use this on memory-constrained screens instead of retaining the full
   * fixed-size metadata structure for every work.
   */
  static void forEachLibraryInfo(const std::function<void(const Ao3LibraryMetadata&)>& callback);

  /** Streams sidecars until the callback returns false. */
  static void forEachLibraryInfoWhile(const std::function<bool(const Ao3LibraryMetadata&)>& callback);

  /**
   * @brief Finds the sidecar associated with a compact index path hash.
   */
  static bool findLibraryInfoByCacheHash(uint32_t cacheHash, Ao3LibraryMetadata& meta);

  /**
   * @brief Resolves a small visible page of compact-index hashes in one SD scan.
   *
   * The caller supplies parallel arrays. found[i] is set only when metadata for
   * cacheHashes[i] was read successfully. This keeps the rich library browser
   * at three metadata records instead of retaining the whole library in RAM.
   */
  static void findLibraryInfoByCacheHashes(const uint32_t* cacheHashes, size_t count,
                                           Ao3LibraryMetadata* metadata, bool* found);

  /**
   * @brief Quick check to see if any AO3 library info exists.
   */
  static bool hasAnyAo3Fics();

  /**
   * @brief Counts indexed works and persisted AO3-specific reading states
   *        without loading the full library into RAM.
   */
  static Ao3LibrarySummary getLibrarySummary();

  /** Finds later indexed works in the same AO3 series, ordered by series part. */
  static std::vector<std::string> findNextSeriesBooks(const std::string& epubPath, size_t maxCount);

  /** Invalidates the cached Home-screen AO3 counts after index/state changes. */
  static void invalidateSummaryCache();

  /** Updates cached Home counts without rescanning every AO3 sidecar. */
  static void updateSummaryForStateChange(Ao3ReadingState oldState, Ao3ReadingState newState);

  /**
   * @brief Helper to map AO3 string ratings to our char codes.
   */
  static char mapRating(const char* ratingStr);

  /**
   * @brief Helper to map AO3 warning strings to our codes.
   */
  static char mapWarning(const char* warningStr);

  /**
   * @brief Writes a compact record into the unified index file.
   */
  static bool writeIndexRecord(const CompactIndexRecord& rec);

  /**
   * @brief Marks a record as tombstoned (deleted) in the index.
   */
  static bool tombstoneRecord(const std::string& epubPath);

  /** Updates an indexed work and its sidecar after its EPUB path changes. */
  static bool migratePath(const std::string& oldPath, const std::string& newPath);

  /**
   * @brief Tombstones any index record whose epub file or ao3_library_info
   *        sidecar no longer exists on disk (e.g. book was moved/renamed).
   * @return Number of records tombstoned, or -1 on index open failure.
   */
  static int sanitizeIndex();

 private:
  /**
   * @brief Internal parser that handles the HTML streaming and anchor searching.
   */
  static bool parseTitlePage(const Epub& epub,
                             Ao3LibraryMetadata& meta,
                             std::string& scrapedWorkId,
                             std::string& scrapedDate,
                             char* scrapedFandom,
                             char* scrapedRel1,
                             char* scrapedRel2);
};
