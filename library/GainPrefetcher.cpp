/*
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "GainPrefetcher.h"

#include "Debug.h"
#include "LoudnessAnalyzer.h"
#include "Messages.h"

#include <Autolock.h>
#include <Path.h>

#include <algorithm>
#include <cmath>
#include <memory>

GainPrefetcher::GainPrefetcher(const BMessenger &window,
                               const std::atomic<bool> *shuttingDown)
    : fWindow(window), fShuttingDown(shuttingDown),
      fWake(create_sem(0, "gain prefetch wake")), fLock("gain prefetch") {}

GainPrefetcher::~GainPrefetcher() {
  if (fWake >= 0)
    delete_sem(fWake);
}

void GainPrefetcher::SetWanted(const std::vector<BString> &paths,
                               size_t priority) {
  {
    BAutolock guard(fLock);
    fWanted = paths;
    fPriorityCount = std::min(priority, paths.size());
    if (!fCurrent.IsEmpty() && !fCurrentPriority) {
      // A row measured in the background gives way when it scrolled off, and
      // when the playing or next track still needs measuring: those are heard
      // first. A dropped row is measured again later.
      bool drop =
          std::find(paths.begin(), paths.end(), fCurrent) == paths.end();
      for (size_t i = 0; i < fPriorityCount && !drop; i++)
        drop = paths[i] != fCurrent && fTried.count(paths[i]) == 0;
      if (drop)
        fDropCurrent.store(true);
    }
  }
  if (!paths.empty())
    release_sem_etc(fWake, 1, B_DO_NOT_RESCHEDULE);
}

void GainPrefetcher::SetPlayingPath(const BString &path) {
  BAutolock guard(fLock);
  fPlaying = path;
}

void GainPrefetcher::SetWriteToFiles(bool write) {
  BAutolock guard(fLock);
  fWrite = write;
  if (!write)
    fPendingWrites.clear();
}

bool GainPrefetcher::_NextPath(BString &path, bool &priority) {
  BAutolock guard(fLock);
  while (!fWanted.empty()) {
    path = fWanted.front();
    fWanted.erase(fWanted.begin());
    priority = fPriorityCount > 0;
    if (fPriorityCount > 0)
      fPriorityCount--;
    if (fTried.insert(path).second) {
      fCurrent = path;
      fCurrentPriority = priority;
      fDropCurrent.store(false);
      return true;
    }
  }
  return false;
}

void GainPrefetcher::_FlushWrites() {
  std::map<BString, ReplayGainInfo> writes;
  {
    BAutolock guard(fLock);
    for (auto it = fPendingWrites.begin(); it != fPendingWrites.end();) {
      if (it->first == fPlaying) {
        ++it;
        continue;
      }
      writes.insert(*it);
      it = fPendingWrites.erase(it);
    }
  }
  for (const auto &[path, gain] : writes) {
    // A gain tagged by something else since then wins.
    ReplayGainInfo existing;
    if (MetadataTagIO::ReadReplayGain(BPath(path.String()), existing) &&
        existing.HasTrack())
      continue;
    MetadataTagIO::WriteReplayGain(BPath(path.String()), gain);
  }
}

void GainPrefetcher::Run() {
  // Playback and the window come first, on a slow machine especially.
  set_thread_priority(find_thread(nullptr), B_LOW_PRIORITY);

  while (!fShuttingDown->load()) {
    _FlushWrites();

    BString path;
    bool priority = false;
    if (!_NextPath(path, priority)) {
      // Woken by new work; the timeout notices shutdown.
      acquire_sem_etc(fWake, 1, B_RELATIVE_TIMEOUT, 1000000);
      continue;
    }

    std::unique_ptr<LoudnessMeter> meter;
    BString error;
    status_t status = LoudnessAnalyzer::AnalyzeFile(
        path.String(), fShuttingDown, meter, error, &fDropCurrent);
    {
      BAutolock guard(fLock);
      fCurrent.Truncate(0);
      // Dropped (scrolled away): it may be wanted again later.
      if (status == B_CANCELED && !fShuttingDown->load())
        fTried.erase(path);
    }
    if (status == B_CANCELED) {
      if (fShuttingDown->load())
        break;
      DEBUG_PRINT("Gain prefetch: dropped %s\n", path.String());
      continue;
    }
    if (status != B_OK || !std::isfinite(meter->Loudness())) {
      DEBUG_PRINT("Gain prefetch: %s: %s\n", path.String(), error.String());
      continue;
    }

    ReplayGainInfo gain;
    gain.trackGain = (float)meter->Gain();
    gain.trackPeak = (float)meter->Peak();
    DEBUG_PRINT("Gain prefetch: %s: %+.2f dB\n", path.String(),
                (double)gain.trackGain);

    BMessage result(MSG_LOUDNESS_PROGRESS);
    result.AddString("path", path);
    result.AddFloat("rgTrackGain", gain.trackGain);
    result.AddFloat("rgTrackPeak", gain.trackPeak);
    fWindow.SendMessage(&result);

    BAutolock guard(fLock);
    if (fWrite)
      fPendingWrites[path] = gain;
  }
}
