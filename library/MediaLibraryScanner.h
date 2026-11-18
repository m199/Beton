/*
 * Copyright 2005-2026, Daniel Weber.
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#ifndef BETON_MEDIA_LIBRARY_SCANNER_H
#define BETON_MEDIA_LIBRARY_SCANNER_H

#include "MediaItem.h"

#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <Locker.h>
#include <Looper.h>
#include <Message.h>
#include <Messenger.h>
#include <OS.h>
#include <String.h>
#include <atomic>
#include <chrono>
#include <map>
#include <vector>

/**
 * @class MediaLibraryScanner
 * @brief Background worker for recursive directory scanning and metadata
 * extraction.
 *
 * Runs in its own thread (via BLooper and a separate worker thread).
 * Scans a directory tree, identifies audio files, extracts metadata using
 * TagLib, and sends batches of `MediaItem`s to the `MediaLibraryCache` for storage.
 *
 * Supports incremental scanning by checking file modification times against
 * a provided cache map.
 */
class MediaLibraryScanner : public BLooper {
public:
  /**
   * @brief Constructs the scanner.
   * @param startDir Root directory to scan.
   * @param cacheTarget Messenger to receive batched MediaItems
   * (MSG_MEDIA_BATCH) plus workload, progress and completion reports.
   * @param scanId Identifies this scanner in the reports it sends, so the
   * cache can aggregate several scanners without their counts colliding.
   *
   * Everything a scanner produces goes to the cache, which is the only thing
   * that can see all the scanners at once and so the only thing able to give
   * the window one coherent figure.
   */
  MediaLibraryScanner(const entry_ref &startDir, BMessenger cacheTarget,
               int32 scanId = 0);
  virtual ~MediaLibraryScanner();

  void MessageReceived(BMessage *msg) override;

  /**
   * @brief Pre-loads the cache to enable incremental scanning.
   * @param cache Map of existing file paths to MediaItems.
   */
  void SetCache(const std::map<BString, MediaItem> &cache) { fCache = cache; }

  /**
   * @brief Asks the worker to stop, without waiting for it.
   *
   * Split from WaitForStop() so a caller shutting several scanners down can
   * signal them all before blocking on any one of them. That matters: once
   * the flag is set the worker stops sending, so it cannot fill the port of a
   * looper that is already blocked waiting for it.
   */
  void RequestStop();

  /** @brief Id this scanner was launched with, used to retire it. */
  int32 ScanId() const { return fScanId; }

  /**
   * @brief Blocks until the worker thread has actually exited.
   *
   * Safe to call more than once, and safe if the worker already finished.
   */
  void WaitForStop();

private:
  void ProcessFile(BEntry &entry);
  void FlushBatch();
  void ReportProgress();

  /**
   * @brief Decides whether a file still needs its tags read.
   *
   * The single rule behind both the counting pass and the scan itself: a file
   * whose size and modification time still match the cached entry is already
   * correct and is not work. Keeping one definition is what lets the progress
   * counter reach its total exactly.
   */
  bool _NeedsScan(const BString &filePath, const struct stat &st) const;

  /**
   * @brief Walks the tree and counts the actual workload, reading no tags.
   *
   * Files already cached and unchanged are excluded, so the totals describe
   * only what this scan is really going to do. A full walk of a ~3,900 entry
   * library measures around 25 ms, which is nothing beside the tag reads that
   * follow, and it buys an honest percentage and time estimate.
   */
  void CountWorkload();

  static status_t WorkerEntry(void *data);
  void WorkerMethod();

  /** @name Configuration & Messaging */
  ///@{
  entry_ref fStartRef;
  BMessenger fCacheTarget;
  BString fBasePath;
  ///@}

  /** @name Data */
  ///@{
  std::map<BString, MediaItem> fCache;
  std::vector<MediaItem> fBatchBuffer;
  BLocker fBatchLock;
  ///@}

  /** @name Threading */
  ///@{
  thread_id fWorkerThread;
  sem_id fControlSem;
  ///@}

  /** @name State Flags */
  ///@{
  bool fScanRequested;
  std::atomic<bool> fStopRequested;
  std::atomic<bool> fIsScanning;
  ///@}

  /** @name Progress Tracking */
  ///@{
  /** @brief Identifies this scanner's reports to the aggregating cache. */
  int32 fScanId;
  /** @brief Folders holding at least one file that needs scanning. */
  std::atomic<int> fWorkDirs;
  /** @brief Files that need their tags read, counted before the scan starts. */
  std::atomic<int> fWorkFiles;
  std::atomic<int> fScannedDirs;
  std::atomic<int> fFoundFiles;
  std::chrono::steady_clock::time_point fLastUpdate;
  std::chrono::steady_clock::time_point fStartTime;
  ///@}
};

#endif // BETON_MEDIA_LIBRARY_SCANNER_H
