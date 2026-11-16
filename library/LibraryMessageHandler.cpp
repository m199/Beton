/*
 * Copyright 2005-2026, Daniel Weber.
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "LibraryController.h"
#include "LibraryMessageHandler.h"

#include "MainWindow.h"
#include "PlaylistEditController.h"
#include "Messages.h"
#include "PlaylistSelectionController.h"
#include "LibraryBrowserController.h"
#include "MediaTableView.h"
#include "PlaybackQueueManager.h"

#include <Message.h>

LibraryMessageHandler::LibraryMessageHandler(MainWindow *window)
    : fWindow(window) {}

bool LibraryMessageHandler::HandleMessage(BMessage *msg) {
  if (!fWindow || !msg)
    return false;

  switch (msg->what) {
  case MSG_CACHE_LOADED: {
    fWindow->fLibraryController->HandleCacheLoaded();
    break;
  }

  case MSG_CACHE_CORRUPT: {
    fWindow->fLibraryController->HandleCacheCorrupt(msg);
    break;
  }

  case MSG_CACHE_EMPTY: {
    fWindow->fLibraryController->HandleCacheEmpty(msg);
    break;
  }

  case MSG_DELETE_ITEM: {
    fWindow->fPlaylistEditController->DeleteSelectedPlaylistItems();
    break;
  }

  case MSG_RESCAN_FULL: {
    fWindow->fLibraryController->StartFullRescan();
    break;
  }

  case MSG_SCAN_PROGRESS: {
    fWindow->fLibraryController->UpdateScanProgress(msg);
    break;
  }

  case MSG_SCAN_TICK: {
    fWindow->fLibraryController->TickScanStatus();
    break;
  }

  case MSG_SCAN_DONE: {
    fWindow->fLibraryController->HandleScanDone(msg);
    break;
  }

  case MSG_BATCH_TIMER: {
    fWindow->fLibraryController->HandleBatchTimer();
    break;
  }

  case MSG_MEDIA_BATCH: {
    fWindow->fLibraryController->HandleMediaBatch(msg);
    break;
  }

  case MSG_VIEWS_REFRESH_PARTIAL: {
    fWindow->fLibraryController->RefreshPartialViews();
    break;
  }

  case MSG_MEDIA_ITEM_FOUND: {
    fWindow->fLibraryController->HandleMediaItemFound(msg);
    break;
  }

  case MSG_MEDIA_ITEM_REMOVED: {
    fWindow->fLibraryController->HandleMediaItemRemoved(msg);
    break;
  }

  case MSG_FOLDER_ROW_RESTORE: {
    // Undo of a Folder-mode move/trash: reinsert the saved row. Only if we are
    // still viewing the same folder, so we never inject a stale row into an
    // unrelated view.
    BString folder;
    if (msg->FindString("folder", &folder) == B_OK && fWindow->fIsFolderMode &&
        fWindow->fCurrentPlaylistName == folder && fWindow->fLibraryManager) {
      MediaTableView *cv = fWindow->fLibraryManager->ContentView();
      if (cv) {
        // The file may already be back with its row, e.g. restored from the
        // Trash by Tracker before this Undo; don't show it twice.
        MediaItem item = MediaTableView::UnarchiveItem(msg);
        if (!cv->HasPath(item.path)) {
          int32 index = -1;
          msg->FindInt32("index", &index);
          cv->AddEntry(item, index);
        }
      }
    }
    break;
  }

  case MSG_ANALYZE_LOUDNESS: {
    fWindow->fLibraryController->AnalyzeLoudness(msg);
    break;
  }

  case MSG_LOUDNESS_PROGRESS: {
    fWindow->fLibraryController->HandleLoudnessResult(msg);
    break;
  }

  case MSG_CD_RIP_IMPORT: {
    fWindow->fLibraryController->ImportRippedFiles(msg);
    break;
  }

  case MSG_CD_RIP_PLAY: {
    std::vector<MediaItem> items;
    BString path;
    for (int32 i = 0; msg->FindString("path", i, &path) == B_OK; i++) {
      MediaItem item;
      item.path = path;
      msg->FindString("title", i, &item.title);
      msg->FindString("artist", i, &item.artist);
      msg->FindString("album", i, &item.album);
      msg->FindInt32("track", i, &item.track);
      msg->FindInt32("duration", i, &item.duration);
      items.push_back(item);
    }
    int32 index = 0;
    msg->FindInt32("index", &index);
    if (fWindow->fPlaybackQueueManager)
      fWindow->fPlaybackQueueManager->PlayItems(items, index);
    break;
  }

  case MSG_FILE_MOVE: {
    fWindow->fLibraryController->HandleFileMove(msg);
    break;
  }

  case MSG_FILE_MOVED: {
    fWindow->fLibraryController->HandleFileMoved(msg);
    break;
  }

  case MSG_PLAYLIST_SELECTION:
  case MSG_INIT_LIBRARY: {
    if (fWindow->fPlaylistSelectionController)
      fWindow->fPlaylistSelectionController->HandlePlaylistSelection(msg);
    break;
  }

  default:
    return false;
  }

  return true;
}

