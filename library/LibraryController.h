/*
 * Copyright 2005-2026, Daniel Weber.
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#ifndef BETON_LIBRARY_CONTROLLER_H
#define BETON_LIBRARY_CONTROLLER_H

#include <Message.h>
#include <MessageRunner.h>
#include <String.h>

#include <atomic>
#include <vector>

class BFilePanel;
class GainPrefetcher;

class MainWindow;

/**
 * @brief Coordinates library-scanning and cache update flow for the main window.
 *
 * This controller bridges asynchronous cache/scan messages to UI updates,
 * keeps the in-memory media index in sync, and triggers view refreshes.
 */
class LibraryController {
public:
  /**
   * @brief Creates a library controller bound to the given main window.
   * @param window Owning window context.
   */
  LibraryController(MainWindow* window);

  /**
   * @brief Destroys the controller.
   */
  ~LibraryController();

  /**
   * @brief Opens the music-source manager dialog.
   */
  void ShowDirectoryManager();

  /**
   * @brief Reveals one or more files in Tracker.
   * @param msg Message containing `refs` or a nested `files` message with refs.
   */
  void RevealInTracker(BMessage* msg);

  /**
   * @brief Moves a file on disk and updates library state on success.
   * @param msg Message with "from" and "to" absolute path strings.
   * On failure nothing changes and the error is shown in the status bar.
   */
  void HandleFileMove(BMessage* msg);

  /**
   * @brief Follows a file another app moved out of the watched folder.
   * @param msg MSG_FILE_MOVED from the cache, with "from" and "to" paths.
   */
  void HandleFileMoved(BMessage* msg);

  /**
   * @brief Adds files the CD ripper just wrote to the library.
   * @param msg MSG_CD_RIP_IMPORT with one "path" per file.
   */
  void ImportRippedFiles(BMessage* msg);

  /**
   * @brief Removes the selected tracks from the library (and playlists),
   *        after asking. Tracks in sidebar folders are skipped: they leave
   *        the library when their files leave the folder.
   */
  void RemoveSelectedFromLibrary();

  /** @brief Removes tracks whose files are gone, after asking. */
  void RemoveMissingTracks();

  /** @brief Adds a playlist's tracks that aren't in the library yet. */
  void AddPlaylistTracks(const std::vector<BString> &paths);

  /** @brief Redo of a removal: removes the "path"s without asking. */
  void RemovePathsFromLibrary(const BMessage *msg);

  /** @brief Undo of a removal: puts the "path"s back in the library, then
   *         restores the playlists they were taken out of. */
  void RestoreToLibrary(const BMessage *msg);

  /** @brief Opens the File > Add to Library panel. */
  void ShowAddToLibraryPanel();

  /**
   * @brief Adds the files in @p msg's "refs" to the library; folders are
   *        searched with their subfolders, on a background thread.
   */
  void AddRefsToLibrary(const BMessage *msg);

  /**
   * @brief Puts the "refs" dropped on a sidebar folder into that "folder":
   *        moved on the same disk, copied (on a thread) from another one.
   *        Asks once when names are taken; undoable.
   */
  void DropIntoFolder(const BMessage *msg);
  /** @brief The copy thread of DropIntoFolder() has finished. */
  void FolderCopyDone(const BMessage *msg);

  /** @brief True if Tracker's Copy or Cut put files on the clipboard. */
  static bool ClipboardHasFiles();
  /**
   * @brief Pastes the files Tracker copied or cut into @p folder: copied
   *        ones are copied, cut ones moved, as DropIntoFolder() does.
   */
  void PasteIntoFolder(const BString &folder);

  /**
   * @brief Measures the loudness of the selected files on a background
   *        thread and writes ReplayGain tags into them.
   * @param msg MSG_ANALYZE_LOUDNESS with a "files" message of refs.
   *
   * Track gain is written for every file. Album gain is written only when
   * the selection holds every file of that album in its folder, so it never
   * describes part of an album.
   */
  void AnalyzeLoudness(BMessage* msg);

  /**
   * @brief Stores gain values the loudness analysis wrote into a file.
   * @param msg MSG_LOUDNESS_PROGRESS with "path" and rg* floats.
   */
  void HandleLoudnessResult(BMessage* msg);

  /**
   * @brief Tells the GainPrefetcher which tracks to measure: while local
   *        music plays, the playing and next tracks and the rows on screen
   *        that have no track gain. Called on a timer, when the setting
   *        changes, when a track starts and when the list is refilled.
   * @param listChanged The list was just refilled: queue its rows at once
   *        instead of waiting a tick to see that it isn't being scrolled.
   */
  void UpdateGainPrefetch(bool listChanged = false);

  /**
   * @brief Handles initial cache-load completion and refreshes views.
   */
  void HandleCacheLoaded();

  /**
   * @brief Rebuilds the library after the cache file was found to be corrupt.
   *
   * The cache looper has already moved the bad file aside and reported
   * whatever it could salvage from it. This kicks off a rescan of every
   * configured source and sidebar folder, plus an explicit scan of the tracks
   * referenced by saved playlists, so the gaps get filled in.
   *
   * If nothing was salvageable the views are cleared first; when entries were
   * recovered they are kept, because the scan fast-skips those files and will
   * not re-report them.
   *
   * @param msg Message carrying the "recovered"/"declared" counts and the
   *            "backup" path of the file that was set aside.
   */
  void HandleCacheCorrupt(BMessage *msg);

  /**
   * @brief Rebuilds the library when the cache loaded but held no entries.
   *
   * Covers a cache file that is missing entirely as well as one that reads
   * back with zero records. Nothing else at startup would notice, so a user
   * with configured folders or saved playlists would be left staring at an
   * empty library until they rescanned by hand. Does nothing when there is
   * genuinely nothing configured to scan.
   *
   * @param msg Message carrying the configured source-directory count.
   */
  void HandleCacheEmpty(BMessage *msg);

  /**
   * @brief Starts a full media rescan and clears visible list views.
   */
  void StartFullRescan();

  /**
   * @brief Takes a new aggregated progress report from the cache.
   * @param msg Message with folder/playlist scope, file counts and elapsed time.
   */
  void UpdateScanProgress(BMessage* msg);

  /**
   * @brief Redraws the scan status from the last report.
   *
   * Driven by a one-second timer so the clock keeps moving between reports and
   * the trailing time label can alternate on schedule.
   */
  void TickScanStatus();

  /**
   * @brief Finalizes rescan state and reloads library entries from cache.
   * @param msg Scan-done message with elapsed time metadata.
   */
  void HandleScanDone(BMessage* msg);

  /**
   * @brief Processes a cache-loading batch timer tick.
   */
  void HandleBatchTimer();

  /**
   * @brief Applies a batch of media item updates from cache/scanner.
   * @param msg Batch message containing repeated item fields.
   */
  void HandleMediaBatch(BMessage* msg);

  /**
   * @brief Performs a debounced partial refresh of filtered list views.
   */
  void RefreshPartialViews();

  /**
   * @brief Applies an incremental single-item metadata update.
   * @param msg Message containing item path and updated fields.
   */
  void HandleMediaItemFound(BMessage* msg);

  /**
   * @brief Removes a media item from views and in-memory index.
   * @param msg Message containing the item path.
   */
  void HandleMediaItemRemoved(BMessage* msg);

  /**
   * @brief Rebuilds the path-to-index lookup for `fAllItems`.
   */
  void RebuildPathIndex();

private:
  static status_t _AddToLibraryThread(void *data);
  BFilePanel *fAddPanel = nullptr;

  std::vector<BString> _SidebarFolders() const;
  /** Removes @p paths from the library and from playlists; @p recordUndo
   *  adds the removal to Edit > Undo (not when replaying a Redo). */
  void _RemoveFromLibrary(const std::vector<BString> &paths,
                          bool recordUndo = true);

  /** True while an AnalyzeLoudness() thread is running. */
  std::atomic<bool> fLoudnessRunning{false};

  /** Measures gain in the background; created when first needed. */
  GainPrefetcher *fGainPrefetcher = nullptr;
  /** Top visible row at the last gain prefetch tick, to see scrolling. */
  BString fGainVisibleTop;

  /**
   * @brief Scans every configured source, sidebar folder and saved playlist.
   *
   * Shared by the corrupt- and empty-cache paths. Playlist tracks are queued
   * by path on top of the folder scan, because a playlist may reference files
   * that live outside every scanned folder.
   */
  void _StartLibraryRebuild();

  /**
   * @brief Tells the user their cache was damaged and what is being done.
   *
   * Shown asynchronously: the rebuild is already running and the window must
   * stay responsive behind the alert.
   *
   * @param recovered Entries salvaged from the damaged file (may be 0).
   * @param declared Entry count the damaged file claimed to hold.
   * @param backupPath Where the damaged file was kept, empty if it was not.
   */
  void _ShowCacheDamagedAlert(int32 recovered, int32 declared,
                              const BString &backupPath);

  /**
   * @brief Counts sidebar entries that stand for a collection of files.
   * @return Number of Folder and Playlist items in the sidebar.
   */
  int32 _CountSidebarCollections() const;

  /**
   * @brief Renders the scan status line from the stored snapshot.
   *
   * The trailing label alternates between elapsed and estimated remaining
   * time every five seconds, so both are available without needing room for
   * both at once.
   */
  void _RenderScanStatus();

  /** @brief Starts the one-second status timer if it is not already running. */
  void _StartScanStatusTimer();

  /** @brief Stops the status timer once a scan cycle is over. */
  void _StopScanStatusTimer();

  /** @brief Owning main window and shared state access. */
  MainWindow* fWindow;

  /** @name Scan status snapshot */
  ///@{
  /** @brief Folders holding work in the running scan. */
  int32 fScanFolders{0};
  /** @brief Playlists holding work in the running scan. */
  int32 fScanPlaylists{0};
  /** @brief Files scanned so far. */
  int32 fScanFilesDone{0};
  /** @brief Files this scan has to get through. */
  int32 fScanFilesTotal{0};
  /** @brief When the running scan started, for the live elapsed clock. */
  bigtime_t fScanStartTime{0};
  /** @brief Repeating one-second tick, or NULL when no scan is running. */
  BMessageRunner* fScanStatusRunner{nullptr};
  ///@}
};

#endif // BETON_LIBRARY_CONTROLLER_H
