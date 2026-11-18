/*
 * Copyright 2005-2026, Daniel Weber.
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#ifndef BETON_MEDIA_LIBRARY_CACHE_H
#define BETON_MEDIA_LIBRARY_CACHE_H

#include "MediaItem.h"
#include "Messages.h"
#include <Directory.h>
#include <Entry.h>
#include <Looper.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <String.h>
#include <Query.h>
#include <Volume.h>
#include <deque>
#include <map>
#include <set>
#include <vector>

class MediaLibraryScanner;

/**
 * @class MediaLibraryCache
 * @brief Manages the central media library cache.
 *
 * The MediaLibraryCache is responsible for:
 * - Loading and saving the 'media.cache' file.
 * - Coordinating the scanning process (via MediaLibraryScanner).
 * - Maintaining the in-memory state of all known media files (fEntries).
 * - Notifying the UI about progress and updates.
 *
 * It runs as a BLooper to handle asynchronous messages.
 */
class MediaLibraryCache : public BLooper {
public:
  /**
   * @brief Construct a new Cache Manager object
   *
   * @param target The target messenger (usually MainWindow) to receive
   * notifications.
   */
  MediaLibraryCache(const BMessenger &target);

  /**
   * @brief Saves pending cache changes and releases live-query resources.
   */
  ~MediaLibraryCache() override;

  /**
   * @brief Loads the cache from disk.
   */
  void LoadCache();

  /**
   * @brief Saves the current cache to disk.
   */
  void SaveCache();

  /**
   * @brief Starts the scanning process for all configured directories.
   *
   * Deferred if any scanner is still running; see the note on fRescanPending.
   */
  void StartScan();

  /**
   * @brief Scans a single directory path asynchronously.
   * @param dirPath Directory path to scan.
   */
  void ScanPath(const BString &dirPath);

  /**
   * @brief Starts watching a folder and its subdirectories recursively for updates.
   * @param folderPath Directory path to watch.
   */
  void StartWatchingFolder(const BString &folderPath);

  /**
   * @brief Stops watching all folders/files currently watched.
   */
  void StopWatchingFolder();

  /**
   * @brief Handles cache/scanner/query messages on the looper thread.
   * @param msg Incoming message.
   */
  void MessageReceived(BMessage *msg) override;

  /**
   * @brief Returns the internal path-indexed cache map.
   */
  const std::map<BString, MediaItem> &Entries() const { return fEntries; }

  /**
   * @brief Returns a flattened vector of all media items.
   * Useful for UI population.
   */
  std::vector<MediaItem> AllEntries() const;

  /**
   * @brief True if @p path lies in the Trash of any mounted volume.
   *
   * Tracks moved to the Trash leave the library; the cache keeps their entry
   * until the next full scan so that Undo can bring them back.
   */
  static bool IsInTrash(const BString &path);

  /** @brief True if @p path has an extension the library plays. */
  static bool IsAudioFile(const BString &path) {
    return _IsSupportedAudioFile(path);
  }

  /**
   * @brief Loads configured source directories into `outDirs`.
   * @param outDirs Output vector with absolute directory paths.
   *
   * Static: it only reads the settings files, so other parts of the app (the
   * CD ripper) can ask which folders make up the library.
   */
  static void LoadDirectories(std::vector<BString> &outDirs);

private:
  /** @brief Outcome of reading the on-disk cache file. */
  enum CacheLoadResult {
    kCacheOk,       ///< File read cleanly, or no cache file exists yet.
    kCacheSalvaged, ///< File was damaged, but usable entries were recovered.
    kCacheCorrupt,  ///< Nothing usable could be read; must be discarded.
  };

  /** @brief What a salvage pass over a damaged cache file managed to do. */
  struct SalvageReport {
    uint32 declared{0};  ///< Entry count claimed by the file header.
    uint32 recovered{0}; ///< Records that parsed and validated.
    uint32 dropped{0};   ///< Recovered records rejected by the on-disk check.
    int32 resyncs{0};    ///< Times the parser had to skip past damage.
  };

  /**
   * @brief Reads and validates the on-disk cache into `fEntries`.
   * @return kCacheCorrupt if the file is unusable, kCacheOk otherwise.
   */
  CacheLoadResult _ReadCacheFile();

  /**
   * @brief Reads the binary (magic 'BTCA') cache format with bounds checks.
   *
   * Damage does not abort the load. Records are self-delimiting, so when one
   * fails to parse the reader hunts for the next intact record boundary and
   * carries on; whatever survives is kept and the rest is left for the scan
   * to fill in. Only a file with nothing salvageable in it is a total loss.
   *
   * @param file Cache file, positioned at offset 0.
   * @param fileSize Size of the cache file in bytes.
   */
  CacheLoadResult _ReadBinaryCache(BFile &file, off_t fileSize);

  /**
   * @brief Finds the next intact record after damage at/after `after`.
   *
   * Scans forward for a plausible record header and confirms the guess by
   * parsing a whole record there, so a chance byte pattern cannot drag the
   * reader off into nonsense.
   * @return Offset of the next parseable record, or -1 if there is none.
   */
  off_t _ResyncToNextRecord(BFile &file, off_t after, off_t fileSize,
                            uint32 version);

  /**
   * @brief Drops salvaged entries that disagree with the file on disk.
   *
   * Metadata recovered from a damaged file is only trustworthy if the record
   * still describes the real file, so each one is checked against stat().
   * Size and mtime are exactly the criteria MediaLibraryScanner uses to skip
   * an unchanged file, so an entry that survives this check is one the
   * upcoming scan can safely fast-skip.
   *
   * Entries that cannot be stat()ed at all are kept: that usually means an
   * unmounted volume, and the existing offline/missing handling deals with
   * them far better than throwing the metadata away would.
   *
   * @return Number of entries dropped.
   */
  uint32 _VerifySalvagedEntries();

  /** @brief Result of the last salvage pass, for logging and the UI. */
  SalvageReport fSalvage;

  /**
   * @brief Where a damaged cache file was set aside, empty if none was kept.
   *
   * Reported to the user so they can find the file for further analysis, so
   * it must only be set when the copy really exists.
   */
  BString fCorruptBackupPath;

  /**
   * @brief Reads the legacy flattened-BMessage cache format.
   * @param file Cache file, positioned at offset 0.
   */
  CacheLoadResult _ReadLegacyCache(BFile &file);

  /**
   * @brief Moves a corrupt cache file out of the way so the app starts clean.
   *
   * The file is renamed to `media.cache.corrupt` (a single slot, overwritten
   * each time) rather than unlinked, so a bad cache can still be inspected
   * after the fact. If the rename fails the file is deleted outright — the
   * important part is that the next start does not read it again.
   */
  void _DiscardCorruptCache();

  /**
   * @brief Inserts or replaces an item in the cache.
   * @param entry Item to store.
   */
  void AddOrUpdateEntry(const MediaItem &entry);


  /**
   * @brief Marks all entries below a base path as currently unavailable.
   * @param basePath Root path that is offline.
   */
  void MarkBaseOffline(const BString &basePath);

  /** @name Data */
  ///@{
  /** @brief Path-indexed media entries (`key == MediaItem::path`). */
  std::map<BString, MediaItem> fEntries;
  /** The sidebar's folders. Their tracks mirror the folder: a file gone from
   *  one leaves the library, where other tracks are only marked missing. */
  std::vector<BString> fSidebarFolders;
  /** Entries taken out by Remove from Library, newest last, so Undo can put
   *  them back as they were (a missing track can't be rescanned). Kept for
   *  as many removals as the undo history holds; memory only. */
  std::deque<std::map<BString, MediaItem>> fRemovedBatches;
  /** @brief UI/update target messenger (usually `MainWindow`). */
  BMessenger fTarget;
  /** @brief Absolute path of the on-disk cache file. */
  BString fCachePath;
  /**
   * @brief Number of scanner loopers that have not reported completion yet.
   *
   * Every scanner this class launches sends exactly one MSG_SCAN_DONE back, so
   * this is an honest count of live scanners. Nothing may reset it: a scan
   * that zeroes the counter while earlier scanners are still running makes
   * their completions decrement the wrong scan, which drives the count
   * negative and fires "all scanners finished" early.
   */
  int32 fActiveScanners{0};

  /**
   * @brief Set when a full scan was requested while scanners were running.
   *
   * A full scan prunes entries for unmonitored and vanished files, so running
   * one while another scan is still reporting would have the two passes fight
   * over the same entries. The request is held here instead and run once the
   * last scanner finishes; any number of requests that pile up in the
   * meantime collapse into a single scan.
   */
  bool fRescanPending{false};
  bool fCacheDirty{false}; ///< Set when entries changed, cleared after SaveCache()
  
  /** @brief Active rating live queries (owned pointers). */
  std::vector<BQuery*> fRatingQueries;
  /** @brief Volumes that already have initialized rating live queries. */
  std::set<dev_t> fQueriedVolumes;
  ///@}

  /**
   * @brief Initializes rating live queries for all configured source volumes.
   */
  void _InitAllLiveQueries();

  /**
   * @brief Initializes per-rating live queries on one BFS-capable volume.
   * @param device Device identifier.
   */
  void _InitRatingLiveQueries(dev_t device);

  /**
   * @brief Re-reads BFS metadata attributes and updates an item in-place.
   * @param item Item to refresh.
   * @return `true` if at least one attribute changed.
   */
  bool _RereadBfsAttributes(MediaItem &item);

  /** Watches every folder in the sidebar folders, so files added, removed
   *  or moved there reach the library while Beton runs. */
  void _WatchSidebarFolders();
  /** Watches @p dir and the folders below it; adds the audio files found to
   *  @p audioFiles when given. */
  void _WatchDirTree(const BEntry &dir, std::vector<BString> *audioFiles);
  /** Stops watching @p dir and the folders below it. */
  void _UnwatchDirTree(const BEntry &dir);
  /** Watches the audio files below @p dir for changes (the open folder). */
  void _WatchFilesRecursive(BDirectory &dir);
  void _WatchFile(const BEntry &file);
  /** The path of folder @p dir, or false if it can't be found. */
  static bool _DirPath(dev_t device, ino_t dir, BString &out);
  /** The library key of the file @p name in folder @p dir (node @p node),
   *  or an empty string if it isn't in the library. */
  BString _FindEntryKey(dev_t device, ino_t dir, const char *name,
                        ino_t node) const;
  /** Moves a library entry to a new path, keeping what it knows. */
  void _RekeyEntry(const BString &from, const BString &to);
  void _HandleNodeMonitor(BMessage *msg, int32 opcode);
  void _HandleEntryArrived(const entry_ref &ref);
  bool _IsWatched(const node_ref &nref) const;
  void _ScanAndAddFile(const BString &pathStr);
  void _RemoveFileFromCache(const BString &pathStr);
  static bool _IsSupportedAudioFile(const BString &path);

  /**
   * @brief Marks the cache dirty and throttles the write-back to disk.
   *
   * Node-monitor events can arrive in bursts (e.g. dropping hundreds of files
   * into a watched folder). Writing the whole cache per event is O(n) each
   * time, so instead arm a one-shot runner and coalesce: at most one SaveCache()
   * per kSaveThrottleDelay while events keep coming.
   */
  void _ScheduleSave();

  /**
   * @brief Drops cache entries under `folderPath` whose file no longer exists.
   *
   * The scanner only reports files it finds, so deletions that happened while
   * the folder was not being watched would otherwise linger until a full
   * rescan. Called on folder entry, before the catch-up scan.
   * @param folderPath Folder root to reconcile.
   */
  void _ReconcileFolderDeletions(const BString &folderPath);

  /**
   * @brief Ends a scan cycle: runs deferred work, flushes, and reports.
   *
   * Shared by the "nothing to scan" shortcut in StartScan() and by the last
   * scanner's completion, so both paths pick up a deferred rescan, drain any
   * queued single-file work, and report exactly one MSG_SCAN_DONE.
   */
  void _FinishScanCycle();

  /**
   * @brief Queues explicit file paths for (re)caching.
   *
   * Used by the corrupt-cache recovery: playlist tracks may live outside every
   * configured source directory, so a folder scan alone would not bring them
   * back. Paths are drained in chunks once the folder scanners are done.
   * @param msg Message carrying repeated "path" strings.
   */
  void _QueueFileScans(BMessage *msg);

  /**
   * @brief Scans the next chunk of `fPendingFileScans`, then reposts itself.
   *
   * Reading tags is slow, so the queue is processed a few files at a time to
   * keep this looper responsive to scanner batches and node-monitor events.
   */
  void _ProcessFileScanChunk();

  /** Folders watched for files coming and going: every folder in the
   *  sidebar folders, for as long as Beton runs. */
  std::set<node_ref> fWatchedDirs;
  /** Audio files watched for changes: only those in the open folder. */
  std::set<node_ref> fWatchedFiles;
  /** The folder open in the sidebar, or empty. */
  BString fShownFolder;
  /** Folders that couldn't be watched (the node monitor limit). */
  int32 fWatchFailures = 0;
  /** False until the window has sent the sidebar folders once. */
  bool fSidebarFoldersKnown = false;
  /** Where Beton is moving files itself (Move File To). A track moved out of
   *  a sidebar folder that way stays in the library; moved any other way,
   *  it leaves. */
  std::set<BString> fExpectedMoves;
  /** @brief Armed one-shot save runner, or NULL when no save is pending. */
  BMessageRunner *fSaveThrottle{nullptr};

  /**
   * @brief Begins a scan cycle if one is not already running.
   *
   * Resets the aggregated counters and starts the clock that the elapsed and
   * remaining time estimates are built from.
   */
  void _BeginScanCycle();

  /**
   * @brief Filters the queued playlist paths down to real work.
   *
   * Run when the queue is about to be drained rather than when it is filled:
   * by then the folder scanners have finished, so a track they already
   * refreshed is correctly recognised as needing nothing.
   */
  void _PrepareFileScanQueue();

  /**
   * @brief Whether a path is missing from the cache or no longer matches disk.
   */
  bool _NeedsScan(const BString &path) const;

  /**
   * @brief Publishes one aggregated progress report to the UI.
   *
   * Throttled, and the single place scan progress reaches the window: the
   * per-scanner reports are summed here so the figures cannot jump backwards
   * when a different scanner happens to report next.
   */
  void _EmitScanProgress(bool force = false);

  /** @name Aggregated scan progress */
  ///@{
  /** @brief Workload and progress reported by one scanner. */
  struct ScanCounters {
    int32 folders{0};   ///< Folders holding work.
    int32 filesTotal{0};///< Files needing a scan.
    int32 filesDone{0}; ///< Files scanned so far.
  };

  /**
   * @brief Retires one finished scanner, blocking until its thread is gone.
   * @param scanId Id the scanner was launched with.
   */
  void _DestroyScanner(int32 scanId);

  /**
   * @brief Stops and destroys every live scanner.
   *
   * Called during teardown. Scanners run their own worker threads deep inside
   * tag parsing; letting the team exit around them is what produced the
   * crashes on quitting mid-scan. Every scanner is signalled first and only
   * then waited on, so a worker cannot block sending to this looper while
   * this looper is blocked waiting for that worker.
   */
  void _StopAllScanners();

  /** @brief Live scanners, keyed by the id handed out at launch. */
  std::map<int32, MediaLibraryScanner *> fScanners;
  /// Folder each ScanPath() scanner is working on, so selecting a folder
  /// again while it is still being scanned doesn't start another full scan.
  std::map<int32, BString> fFolderScanPaths;

  /** @brief Per-scanner counters, keyed by the id handed out at launch. */
  std::map<int32, ScanCounters> fScanCounters;
  /** @brief Next scanner id; only has to be unique within a cycle. */
  int32 fNextScanId{1};
  /** @brief True between the first scanner starting and the cycle finishing. */
  bool fScanCycleActive{false};
  /** @brief Start of the current cycle, for elapsed/remaining estimates. */
  bigtime_t fScanStartTime{0};
  /** @brief Last time a progress report was published, for throttling. */
  bigtime_t fLastProgressSent{0};
  /** @brief Playlists contributing at least one track that needs scanning. */
  int32 fQueuedPlaylists{0};
  /** @brief Queued playlist tracks scanned so far this cycle. */
  int32 fQueuedFilesDone{0};
  ///@}

  /** @name Deferred single-file scan queue */
  ///@{
  /** @brief Paths waiting to be tag-scanned, oldest first. */
  std::vector<BString> fPendingFileScans;
  /** @brief Sidebar playlist index each queued path came from. */
  std::vector<int32> fPendingFileScanPlaylists;
  /** Queued paths to read again even though the library has them (a rip
   *  writes tags and loudness after the file appears). */
  std::set<BString> fRefreshPaths;
  /** @brief Read cursor into `fPendingFileScans`. */
  size_t fFileScanIndex{0};
  /** @brief True while chunks are being processed (a step message is in flight). */
  bool fFileScanRunning{false};
  ///@}
};

#endif // BETON_MEDIA_LIBRARY_CACHE_H
