/*
 * Copyright 2005-2026, Daniel Weber.
 * Copyright 2026, David Beckman, indispensablelabs@gmail.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#ifndef BETON_PLAYBACK_QUEUE_MANAGER_H
#define BETON_PLAYBACK_QUEUE_MANAGER_H

#include <SupportDefs.h>
#include <String.h>
#include <map>
#include <random>
#include <vector>

#include "MediaItem.h"

class MediaTableView;
class MainWindow;

/**
 * @class PlaybackQueueManager
 * @brief Controls queue navigation, repeat/shuffle, and source-aware next/prev.
 */
class PlaybackQueueManager {
public:
  /** @brief Repeat policy for queue progression. */
  enum RepeatMode { RepeatOff, RepeatAll, RepeatOne };
  
  /** @brief Logical playback source currently driving queue behavior. */
  enum PlaybackSource {
    SourceNone,
    SourceLibrary,
    SourcePlaylist,
    SourceRadio,
    SourceDLNA
  };

  /**
   * @brief Creates queue manager bound to main window state.
   * @param window Owning main window context.
   */
  explicit PlaybackQueueManager(MainWindow *window);

  /** @brief Builds queue from current selection and starts playback. */
  void PlaySelectedQueue();
  
  /** @brief Plays next item according to source, repeat, and shuffle rules. */
  void PlayNext();
  
  /** @brief Plays previous item according to source and shuffle history. */
  void PlayPrevious();
  
  /** @brief Handles end-of-track progression. */
  void HandleTrackEnded();

  /**
   * @brief Called when the engine could not open @p path because the file
   *        is gone. Marks the track missing and moves on to the next one, as
   *        if it had ended.
   * @return false when playback should stop instead: not a Library or
   *         playlist queue, Repeat One, or every track in the queue has been
   *         skipped in a row.
   */
  bool SkipMissingTrack(const BString &path);

  /**
   * @brief A track started playing: restarts the missing-track skip count,
   *        and skipping goes forward again until the next "previous".
   */
  void ResetMissingSkips() {
    fMissingSkips = 0;
    fStepDirection = 1;
  }
  
  /** @brief Toggles shuffle mode and updates shuffle icon state. */
  void ToggleShuffle();
  
  /** @brief Cycles repeat mode and updates repeat icon state. */
  void ToggleRepeat();
  
  /** @brief Returns whether shuffle mode is enabled. */
  bool ShuffleEnabled() const;
  
  /** @brief Returns repeat mode as persisted integer value. */
  int32 RepeatModeValue() const;
  
  /** @brief Sets shuffle mode from persisted/apply state. */
  void SetShuffleEnabled(bool enabled);
  
  /** @brief Sets repeat mode from persisted/apply state. */
  void SetRepeatModeValue(int32 mode);
  
  /** @brief Sets active playback source for navigation behavior. */
  void SetActiveSource(PlaybackSource source);
  
  /** @brief Plays current selection depending on active app mode. */
  void PlayCurrentSelection();
  
  /** @brief Plays a concrete local row selection from content view. */
  void PlayLocalSelection(MediaTableView *view, int32 rowIndex);

  /**
   * @brief Plays files that need not be in the library or the content view
   *        (freshly ripped tracks), starting at @p index.
   */
  void PlayItems(const std::vector<MediaItem> &items, int32 index);

  /**
   * @brief Details of a queued track that is not in the library index
   * (audio CD, folder outside the library), or nullptr.
   */
  const MediaItem *QueueItemForPath(const BString &path) const;

  /**
   * @brief Queue index of the local track that will play after the current
   *        one, or -1. With shuffle on, this picks the next track now.
   */
  int32 PeekNextIndex();

private:
  /** @brief Returns random queue index in range `[0, count)`. */
  int32 _RandomIndex(int32 count);
  /** @brief The shuffle pick PeekNextIndex() made, else a random index. */
  int32 _TakeShuffleNext(int32 count);
  /** @brief Updates shuffle button icon to current shuffle state. */
  void _UpdateShuffleIcon();
  /** @brief Updates repeat button icon to current repeat state. */
  void _UpdateRepeatIcon();
  /** @brief Ensures play/pause button shows pause while playing. */
  void _SetPlayPauseIcon();
  /** @brief Clears active DLNA play queue context. */
  void _ClearDlnaQueue();
  /**
   * @brief Marks a track whose file is gone as missing in the Library and
   *        in @p view (so its row shows the missing-file sign right away),
   *        then tells the user.
   */
  void _ReportMissingFile(MediaTableView *view, const MediaItem &item);
  /** @brief Sets MediaItem::missing for @p path in the Library and view. */
  void _MarkMissing(const MediaItem &item, MediaTableView *view);
  /** @brief Shows "Skipped missing file …" in the status bar. */
  void _ReportSkipped(const BString &path);
  /** @brief Plays adjacent DLNA item; returns `true` if handled. */
  bool _PlayAdjacentDlna(int32 direction);
  /** @brief Plays adjacent radio station; returns `true` if handled. */
  bool _PlayAdjacentRadio(int32 direction);

  /** @brief Owning main window and shared playback/UI state access. */
  MainWindow *fWindow;
  
  /** @brief Current source context for queue-navigation decisions. */
  PlaybackSource fActiveSource = SourceNone;
  
  /** @brief Shuffle mode flag. */
  bool fShuffleEnabled = false;
  
  /** @brief Active repeat mode. */
  RepeatMode fRepeatMode = RepeatOff;

  /** @brief Missing tracks skipped in a row; stops a queue of only those. */
  int32 fMissingSkips = 0;

  /**
   * @brief Direction a missing track is skipped in: -1 after "previous" (so
   *        stepping back passes it instead of returning to where you were),
   *        else 1.
   */
  int32 fStepDirection = 1;
  
  /** @brief Random generator used for shuffle picks. */
  std::mt19937 fRng{std::random_device{}()};
  
  /** @brief Stack-like history for shuffle previous navigation. */
  std::vector<int32> fShuffleHistory;

  /** @brief Next shuffle pick made ahead by PeekNextIndex(); -1 = none. */
  int32 fShuffleNext = -1;

  /** @brief Queued tracks the library index does not know, by path. */
  std::map<BString, MediaItem> fQueueItems;
};

#endif // BETON_PLAYBACK_QUEUE_MANAGER_H
