/*
 * Copyright 2026, David Beckman, indispensablelabs@gmail.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#ifndef BETON_GAIN_PREFETCHER_H
#define BETON_GAIN_PREFETCHER_H

#include "MetadataTagIO.h"

#include <Locker.h>
#include <Messenger.h>
#include <OS.h>
#include <String.h>

#include <atomic>
#include <map>
#include <set>
#include <vector>

/**
 * @class GainPrefetcher
 * @brief Measures the track gain of library tracks that have none, a few at
 *        a time while music plays, so the library fills in by itself.
 *
 * The window hands it the tracks it wants measured, most wanted first (the
 * next track, then the rows on screen). One worker thread measures them below
 * normal priority and reports each result as MSG_LOUDNESS_PROGRESS, which
 * stores it in the library. When writing to files is on, the gain is also
 * written into the file's tags, but never into the file that is playing:
 * that write waits until another track plays.
 */
class GainPrefetcher {
public:
  GainPrefetcher(const BMessenger &window,
                 const std::atomic<bool> *shuttingDown);
  ~GainPrefetcher();

  /**
   * @brief Tracks to measure, most wanted first; replaces the previous list.
   * @param priority How many leading paths must not be interrupted (the next
   *        and current tracks). A track from further down that is being
   *        measured and is no longer wanted (scrolled away) is dropped.
   */
  void SetWanted(const std::vector<BString> &paths, size_t priority = 0);
  void SetPlayingPath(const BString &path);
  void SetWriteToFiles(bool write);

  /** The worker thread's body; returns once the window shuts down. */
  void Run();

private:
  bool _NextPath(BString &path, bool &priority);
  void _FlushWrites();

  BMessenger fWindow;
  const std::atomic<bool> *fShuttingDown;
  sem_id fWake;
  BLocker fLock;
  // Guarded by fLock:
  std::vector<BString> fWanted;
  size_t fPriorityCount = 0;      ///< Leading fWanted entries not to drop.
  BString fCurrent;               ///< Being measured; empty when idle.
  bool fCurrentPriority = false;
  std::set<BString> fTried; ///< Measured or failed this session.
  std::map<BString, ReplayGainInfo> fPendingWrites;
  BString fPlaying;
  bool fWrite = false;

  std::atomic<bool> fDropCurrent{false}; ///< Stop measuring fCurrent.
};

#endif // BETON_GAIN_PREFETCHER_H
