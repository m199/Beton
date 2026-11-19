/*
 * Copyright 2005-2026, Daniel Weber.
 * Copyright 2026, David Beckman, indispensablelabs@gmail.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "PlaybackMessageHandler.h"

#include "LibraryController.h"
#include "MainWindow.h"
#include "Messages.h"
#include "PlaybackTransportController.h"
#include "PlaybackQueueManager.h"

#include <MenuItem.h>
#include <Message.h>

/**
 * @brief Constructs playback message handler.
 * @param window Owning main window context.
 */
PlaybackMessageHandler::PlaybackMessageHandler(MainWindow *window)
    : fWindow(window) {}

/**
 * @brief Dispatches playback messages to queue/transport controllers.
 * @param msg Incoming message.
 * @return `true` if handled.
 */
bool PlaybackMessageHandler::HandleMessage(BMessage *msg) {
  if (!fWindow || !msg)
    return false;

  switch (msg->what) {
  case MSG_PLAYPAUSE:
    if (fWindow->fPlaybackTransportController)
      fWindow->fPlaybackTransportController->TogglePlayPause();
    return true;

  case MSG_PLAY_BTN: {
    if (fWindow->fPlaybackQueueManager)
      fWindow->fPlaybackQueueManager->PlaySelectedQueue();
    return true;
  }

  case MSG_PLAY_NEXT:
    if (fWindow->fPlaybackQueueManager)
      fWindow->fPlaybackQueueManager->PlayNext();
    return true;

  case MSG_PREV_SONG:
  case MSG_PREV_BTN:
    if (fWindow->fPlaybackQueueManager)
      fWindow->fPlaybackQueueManager->PlayPrevious();
    return true;

  case MSG_PAUSE:
    if (fWindow->fPlaybackTransportController)
      fWindow->fPlaybackTransportController->TogglePause();
    return true;

  case MSG_STOP:
    if (fWindow->fPlaybackTransportController)
      fWindow->fPlaybackTransportController->StopPlayback();
    return true;

  case MSG_SEEK_REQUEST:
    if (fWindow->fPlaybackTransportController)
      fWindow->fPlaybackTransportController->HandleSeekRequest(msg);
    return true;

  case MSG_TIME_UPDATE: {
    if (fWindow->fPlaybackTransportController)
      fWindow->fPlaybackTransportController->UpdatePlaybackTime();
    return true;
  }

  case MSG_TRACK_ENDED:
    if (fWindow->fPlaybackQueueManager)
      fWindow->fPlaybackQueueManager->HandleTrackEnded();
    return true;

  case MSG_MUTE_TOGGLE: {
    if (fWindow->fPlaybackTransportController)
      fWindow->fPlaybackTransportController->ToggleMute();
    return true;
  }

  case MSG_VOLUME_CHANGED: {
    if (fWindow->fPlaybackTransportController)
      fWindow->fPlaybackTransportController->ApplyVolumeChange();
    return true;
  }

  case MSG_DLNA_VOLUME_UPDATE: {
    if (fWindow->fPlaybackTransportController)
      fWindow->fPlaybackTransportController->ApplyDlnaVolumeUpdate(msg);
    return true;
  }

  case MSG_SHUFFLE_TOGGLE: {
    if (fWindow->fPlaybackQueueManager)
      fWindow->fPlaybackQueueManager->ToggleShuffle();
    return true;
  }

  case MSG_GAIN_PREFETCH_TICK:
    if (fWindow->fLibraryController)
      fWindow->fLibraryController->UpdateGainPrefetch(
          msg->GetBool("list_changed", false));
    return true;

  case MSG_GAIN_PREFETCH_TOGGLE:
  case MSG_GAIN_WRITE_TOGGLE: {
    bool &setting = msg->what == MSG_GAIN_PREFETCH_TOGGLE
                        ? fWindow->fGainPrefetch
                        : fWindow->fGainWriteFiles;
    BMenuItem *item = msg->what == MSG_GAIN_PREFETCH_TOGGLE
                          ? fWindow->fGainPrefetchItem
                          : fWindow->fGainWriteItem;
    setting = !setting;
    if (item)
      item->SetMarked(setting);
    if (fWindow->fLibraryController)
      fWindow->fLibraryController->UpdateGainPrefetch();
    fWindow->SaveSettings();
    return true;
  }

  case MSG_REPLAYGAIN_MODE: {
    int32 mode;
    if (msg->FindInt32("mode", &mode) != B_OK)
      mode = AudioPlaybackEngine::kReplayGainAuto;
    fWindow->SetReplayGainMode(mode);
    fWindow->SaveSettings();
    return true;
  }

  case MSG_REPEAT_TOGGLE: {
    if (fWindow->fPlaybackQueueManager)
      fWindow->fPlaybackQueueManager->ToggleRepeat();
    return true;
  }

  case MSG_RADIO_PLAY:
  case MSG_PLAY: {
    if (fWindow->fPlaybackQueueManager)
      fWindow->fPlaybackQueueManager->PlayCurrentSelection();
    break;
  }

  case MSG_NOW_PLAYING: {
    if (fWindow->fPlaybackQueueManager)
      fWindow->fPlaybackQueueManager->ResetMissingSkips();
    if (fWindow->fPlaybackTransportController)
      fWindow->fPlaybackTransportController->HandleNowPlaying(msg);
    // Measure a new track without gain now, not at the next tick.
    if (fWindow->fLibraryController)
      fWindow->fLibraryController->UpdateGainPrefetch();
    break;
  }

  case MSG_PLAY_FAILED: {
    if (fWindow->fPlaybackTransportController)
      fWindow->fPlaybackTransportController->HandlePlayFailed(msg);
    break;
  }

  default:
    return false;
  }

  return true;
}
