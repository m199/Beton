/*
 * Copyright 2005-2026, Daniel Weber.
 * Copyright 2026, David Beckman, indispensablelabs@gmail.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "MetadataMessageHandler.h"

#include "ArtworkController.h"
#include "LibraryController.h"
#include "MainWindow.h"
#include "MediaTableView.h"
#include "Messages.h"
#include "MusicBrainzLookupController.h"
#include "PlaybackTransportController.h"
#include "PropertiesController.h"

#include <Message.h>

/**
 * @brief Constructs metadata message handler and MusicBrainz sub-controller.
 * @param window Owning main window context.
 */
MetadataMessageHandler::MetadataMessageHandler(MainWindow *window)
    : fWindow(window),
      fMusicBrainzLookupController(new MusicBrainzLookupController(window)) {}

/**
 * @brief Destroys metadata message handler.
 */
MetadataMessageHandler::~MetadataMessageHandler() {
  delete fMusicBrainzLookupController;
}

/**
 * @brief Dispatches metadata messages to properties/artwork/MusicBrainz logic.
 * @param msg Incoming message.
 * @return `true` if handled by this router.
 */
bool MetadataMessageHandler::HandleMessage(BMessage *msg) {
  if (!fWindow || !msg)
    return false;

  switch (msg->what) {
  case MSG_SHOW_COVER_CONTEXT_MENU: {
    BPoint screenPoint;
    if (msg->FindPoint("where", &screenPoint) == B_OK && fWindow->fArtworkController)
      fWindow->fArtworkController->ShowCoverContextMenu(screenPoint);
    break;
  }

  case MSG_COVER_LOAD: {
    if (fWindow->fArtworkController)
      fWindow->fArtworkController->OpenCoverFilePanel();
    break;
  }

  case MSG_MAIN_COVER_LOAD_REF: {
    entry_ref ref;
    if (msg->FindRef("refs", 0, &ref) == B_OK && fWindow->fArtworkController)
      fWindow->fArtworkController->HandleCoverChosen(ref);
    break;
  }

  case MSG_COVER_CLEAR: {
    if (fWindow->fArtworkController)
      fWindow->fArtworkController->HandleMainCoverClear();
    break;
  }

  case MSG_COVER_APPLY_ALBUM: {
    if (fWindow->fArtworkController) {
      if (msg->HasData("bytes", B_RAW_TYPE))
        fWindow->fArtworkController->ApplyAlbumCover(msg);
      else
        fWindow->fArtworkController->HandleMainCoverApplyAlbum();
    }
    break;
  }

  case MSG_COVER_CLEAR_ALBUM: {
    if (fWindow->fArtworkController) {
      if (msg->HasString("file"))
        fWindow->fArtworkController->ClearAlbumCover(msg);
      else
        fWindow->fArtworkController->HandleMainCoverClearAlbum();
    }
    break;
  }

  case MSG_COVER_DROPPED_APPLY_ALL: {
    if (fWindow->fArtworkController)
      fWindow->fArtworkController->ApplyDroppedCoverToAll(msg);
    break;
  }

  case MSG_PROP_APPLY:
  case MSG_PROP_SAVE: {
    fWindow->fPropertiesController->SavePropertyTags(msg);
    break;
  }

  case MSG_PROP_REQUEST_COVER: {
    if (fWindow->fArtworkController)
      fWindow->fArtworkController->RequestEmbeddedCover(msg);
    break;
  }

  case MSG_PROP_SET_COVER_DATA: {
    const void *buf = nullptr;
    ssize_t sz = 0;
    if (msg->FindData("bytes", B_RAW_TYPE, &buf, &sz) == B_OK && buf && sz > 0 &&
        fWindow->fArtworkController) {
      std::vector<BString> paths;
      BString filePath;
      int32 i = 0;
      while (msg->FindString("file", i++, &filePath) == B_OK) {
        if (!filePath.IsEmpty())
          paths.push_back(filePath);
      }
      if (paths.empty())
        paths = fWindow->fArtworkController->GetMainCoverTargetPaths();

      if (!paths.empty()) {
        BMessage payload(MSG_COVER_DROPPED_APPLY_ALL);
        for (const auto &p : paths)
          payload.AddString("file", p);
        payload.AddData("bytes", B_RAW_TYPE, buf, sz);
        const char *mime = nullptr;
        if (msg->FindString("mime", &mime) == B_OK && mime)
          payload.AddString("mime", mime);
        fWindow->fArtworkController->ApplyDroppedCoverToAll(&payload);
      }
    }
    break;
  }

  case MSG_PROP_CLOSED: {
    fWindow->fMetadataPropertiesWindow = nullptr;
    break;
  }

  case MSG_PROPERTIES: {
    fWindow->fPropertiesController->OpenMetadataPropertiesWindow(msg);
    break;
  }

  case MSG_MB_SEARCH:
  case MSG_MB_CANCEL:
  case MSG_MATCH_RESULT:
  case MSG_MB_SEARCH_COMPLETE:
  case MSG_MB_APPLY:
  case MSG_MB_APPLY_ALBUM:
  case MSG_COVER_FETCH_MB: {
    if (msg->what == MSG_COVER_FETCH_MB && !msg->HasString("file") &&
        fWindow->fArtworkController) {
      std::vector<BString> paths =
          fWindow->fArtworkController->GetMainCoverTargetPaths();
      for (const auto &p : paths)
        msg->AddString("file", p);

      MediaTableView *cv =
          fWindow->fLibraryManager ? fWindow->fLibraryManager->ContentView() : nullptr;
      const MediaItem *mi = cv ? cv->SelectedItem() : nullptr;
      if (!mi && fWindow->fPlaybackTransportController &&
          fWindow->fPlaybackTransportController->NowPlayingIsValid()) {
        mi = fWindow->fPlaybackTransportController->NowPlayingItem();
      }
      if (mi) {
        if (!mi->artist.IsEmpty()) msg->AddString("artist", mi->artist);
        if (!mi->title.IsEmpty()) msg->AddString("title", mi->title);
        if (!mi->album.IsEmpty()) msg->AddString("album", mi->album);
      }
      
      if (!msg->HasMessenger("original_reply_to")) {
        msg->AddMessenger("original_reply_to", BMessenger(fWindow));
      }
    }
    return fMusicBrainzLookupController &&
           fMusicBrainzLookupController->HandleMessage(msg);
  }

  case MSG_SET_RATING: {
    fWindow->fPropertiesController->SetRating(msg);
    break;
  }

  default:
    return false;
  }

  return true;
}
