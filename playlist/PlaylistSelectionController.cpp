/*
 * Copyright 2005-2026, Daniel Weber.
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "PlaylistSelectionController.h"

#include "MediaTableView.h"
#include "AudioPlaybackEngine.h"
#include "DLNAViewController.h"
#include "Debug.h"
#include "NowPlayingInfoPanel.h"
#include "LibraryBrowserController.h"
#include "LibraryController.h"
#include "MainWindow.h"
#include "Messages.h"
#include "MetadataTagIO.h"
#include "CddaTracks.h"
#include "MusicSourceSettings.h"
#include "PlaylistLibrary.h"
#include "PlaylistEditController.h"
#include "RadioStationController.h"
#include "SingleColumnListView.h"

#include <Directory.h>
#include <Entry.h>
#include <GroupView.h>
#include <MenuItem.h>
#include <Message.h>
#include <Node.h>
#include <Path.h>
#include <String.h>
#include <sys/stat.h>
#include <TextControl.h>
#include <vector>

PlaylistSelectionController::PlaylistSelectionController(MainWindow *window)
    : fWindow(window) {}

void PlaylistSelectionController::HandlePlaylistSelection(BMessage *msg) {
  if (!fWindow || !msg)
    return;

  if (msg->what == MSG_INIT_LIBRARY)
    RestoreInitialPlaylistSelection();

  int32 selected = fWindow->fPlaylistLibrary->View()->CurrentSelection();
  DEBUG_PRINT("Current selection index: %ld\n", (long)selected);
  if (selected < 0)
    return;

  BString name = fWindow->fPlaylistLibrary->View()->ItemAt(selected);
  if (name.IsEmpty())
    return;

  fWindow->fCurrentPlaylistName = name;

  PlaylistItemKind kind = PlaylistKindFromSelection(msg, name);
  fWindow->fIsLibraryMode = (kind == PlaylistItemKind::Library);
  fWindow->fIsFolderMode = (kind == PlaylistItemKind::Folder);
  fWindow->fIsRadioMode = (kind == PlaylistItemKind::Radio);
  fWindow->fIsDlnaMode = (kind == PlaylistItemKind::DLNA);
  fWindow->fIsCDMode = (kind == PlaylistItemKind::CD);

  // Fast Edit and rating make no sense for Radio/DLNA/CD (read-only metadata).
  // Temporarily disable it without touching fFastEditEnabled so the user's
  // preference is restored when switching back to an editable source.
  {
    bool editable = !fWindow->fIsRadioMode && !fWindow->fIsDlnaMode && !fWindow->fIsCDMode;
    bool effective = editable && fWindow->fFastEditEnabled;
    if (fWindow->fLibraryManager && fWindow->fLibraryManager->ContentView()) {
      MediaTableView *cv = fWindow->fLibraryManager->ContentView();
      if (!effective)
        cv->CommitCellEdit();
      cv->SetFastEditEnabled(effective);
      cv->SetRatingEditable(editable);
    }
    if (fWindow->fFastEditItem)
      fWindow->fFastEditItem->SetMarked(effective);
  }

  if (fWindow->fSearchField && fWindow->fSearchField->Text()[0] != '\0')
    fWindow->fSearchField->SetText("");

  ApplyPlaylistFilterVisibility();
  ShowSelectedPlaylistSource(name);
  fWindow->UpdateFileInfo();
}

void PlaylistSelectionController::RestoreInitialPlaylistSelection() {
  DEBUG_PRINT("MSG_INIT_LIBRARY: InitialViewMode='%s', "
              "InitialPlaylist='%s'\n",
              fWindow->fInitialViewMode.String(),
              fWindow->fInitialPlaylistName.String());

  int32 res = -1;
  if (!fWindow->fInitialViewMode.IsEmpty()) {
    if (fWindow->fInitialViewMode == "Playlist" &&
        !fWindow->fInitialPlaylistName.IsEmpty()) {
      res = fWindow->fPlaylistLibrary->View()->SelectByName(
          fWindow->fInitialPlaylistName);
    } else if (fWindow->fInitialViewMode == "Folder" &&
               !fWindow->fInitialPlaylistName.IsEmpty()) {
      res = fWindow->fPlaylistLibrary->View()->SelectByName(
          fWindow->fInitialPlaylistName);
    } else {
      res = fWindow->fPlaylistLibrary->View()->SelectByName(
          fWindow->fInitialViewMode);
    }
  }

  if (res < 0) {
    DEBUG_PRINT("Selection failed, defaulting to Library\n");
    fWindow->fPlaylistLibrary->View()->SelectByName("Library");
  }

  fWindow->fInitialViewMode = "";
  fWindow->fInitialPlaylistName = "";
}

PlaylistItemKind PlaylistSelectionController::PlaylistKindFromSelection(
    BMessage *msg, const BString &name) const {
  int32 kindInt = 0;
  if (msg->what == MSG_PLAYLIST_SELECTION &&
      msg->FindInt32("kind", &kindInt) == B_OK) {
    return (PlaylistItemKind)kindInt;
  }

  if (name == "Library")
    return PlaylistItemKind::Library;
  if (fWindow->fPlaylistLibrary &&
      fWindow->fPlaylistLibrary->IsFolderSource(name))
    return PlaylistItemKind::Folder;
  if (name == "Radio")
    return PlaylistItemKind::Radio;
  if (name == "DLNA")
    return PlaylistItemKind::DLNA;
  return PlaylistItemKind::Playlist;
}

void PlaylistSelectionController::ApplyPlaylistFilterVisibility() {
  if (!fWindow->fFilterGroup)
    return;

  bool showFilters = false;
  if (fWindow->fIsLibraryMode)
    showFilters = fWindow->fShowFiltersLibrary;
  else if (fWindow->fIsRadioMode)
    showFilters = fWindow->fShowFiltersRadio;
  else if (fWindow->fIsDlnaMode)
    showFilters = fWindow->fShowFiltersDlna;
  else
    showFilters = fWindow->fShowFiltersPlaylist;

  if (fWindow->fViewFiltersItem)
    fWindow->fViewFiltersItem->SetMarked(showFilters);

  if (showFilters) {
    if (!fWindow->fIsFilterGroupVisible) {
      fWindow->fIsFilterGroupVisible = true;
      fWindow->fFilterGroup->Show();
    }
    return;
  }

  if (fWindow->fLibraryManager) {
    if (fWindow->fLibraryManager->GenreView())
      fWindow->fLibraryManager->GenreView()->DeselectAll();
    if (fWindow->fLibraryManager->ArtistView())
      fWindow->fLibraryManager->ArtistView()->DeselectAll();
    if (fWindow->fLibraryManager->AlbumView())
      fWindow->fLibraryManager->AlbumView()->DeselectAll();
  }
  if (fWindow->fIsFilterGroupVisible) {
    fWindow->fIsFilterGroupVisible = false;
    fWindow->fFilterGroup->Hide();
  }
}

void PlaylistSelectionController::ShowSelectedPlaylistSource(
    const BString &name) {
  if (fWindow->fIsCDMode) {
    ShowCDPlaylistSource(name);
  } else if (fWindow->fIsRadioMode) {
    ShowRadioPlaylistSource();
  } else if (fWindow->fIsDlnaMode) {
    if (fWindow->fDlnaController)
      fWindow->fDlnaController->ShowPlaylistSource();
  } else if (fWindow->fIsLibraryMode) {
    ShowLibraryPlaylistSource();
  } else if (fWindow->fIsFolderMode) {
    ShowFolderPlaylistSource(name);
  } else {
    ShowRegularPlaylistSource(name);
  }
}

void PlaylistSelectionController::ShowCDPlaylistSource(const BString &name) {
  if (fWindow->fDlnaController)
    fWindow->fDlnaController->SetServerFieldVisible(false);
  fWindow->fLibraryManager->ContentView()->SetRadioMode(false);
  fWindow->fLibraryManager->ContentView()->SetFolderMode(false);
  fWindow->fLibraryManager->SetRadioFilterMode(false);

  // Deselect any previous filter column selections so they don't filter out CD tracks
  if (fWindow->fLibraryManager) {
    if (fWindow->fLibraryManager->GenreView())
      fWindow->fLibraryManager->GenreView()->DeselectAll();
    if (fWindow->fLibraryManager->ArtistView())
      fWindow->fLibraryManager->ArtistView()->DeselectAll();
    if (fWindow->fLibraryManager->AlbumView())
      fWindow->fLibraryManager->AlbumView()->DeselectAll();
  }

  BString cdPath = fWindow->fCDPath;
  int32 selected = fWindow->fPlaylistLibrary->View()->CurrentSelection();
  if (selected >= 0 &&
      fWindow->fPlaylistLibrary->View()->KindAt(selected) == PlaylistItemKind::CD) {
    BString selPath = fWindow->fPlaylistLibrary->View()->PathAt(selected);
    if (!selPath.IsEmpty())
      cdPath = selPath;
  }

  // Remember which drive we're browsing so a later volume rescan (or anything
  // else falling back to fCDPath) stays on this disc rather than the first one.
  fWindow->fCDPath = cdPath;

  std::vector<MediaItem> cdTracks = CddaTracks::Read(cdPath);

  fWindow->fLibraryManager->SetActiveItems(cdTracks);
  if (fWindow->fMediaLibraryCache) {
    BMessage watchMsg(MSG_WATCH_FOLDER);
    watchMsg.AddString("path", "");
    BMessenger(fWindow->fMediaLibraryCache).SendMessage(&watchMsg);
  }
  fWindow->UpdateFilteredViews();
}

void PlaylistSelectionController::ShowRadioPlaylistSource() {
  if (fWindow->fDlnaController)
    fWindow->fDlnaController->SetServerFieldVisible(false);
  fWindow->fLibraryManager->ContentView()->SetRadioMode(true);
  fWindow->fLibraryManager->ContentView()->SetFolderMode(false);
  fWindow->fLibraryManager->SetActivePaths({});
  if (fWindow->fMediaLibraryCache) {
    BMessage watchMsg(MSG_WATCH_FOLDER);
    watchMsg.AddString("path", "");
    BMessenger(fWindow->fMediaLibraryCache).SendMessage(&watchMsg);
  }
  if (fWindow->fRadioStationController)
    fWindow->fRadioStationController->ShowStations();
  bool radioIsPlaying =
      fWindow->fPlaybackEngine &&
      (fWindow->fPlaybackEngine->IsPlaying() || fWindow->fPlaybackEngine->IsPaused()) &&
      fWindow->fRadioStationController &&
      fWindow->fRadioStationController->HasActiveStation();
  if (radioIsPlaying && fWindow->fNowPlayingInfoPanel &&
      fWindow->fShowCoverArt && fWindow->fRadioStationController->ActiveCover()) {
    fWindow->fNowPlayingInfoPanel->SetCover(
        fWindow->fRadioStationController->ActiveCover());
  }
}

void PlaylistSelectionController::ShowLibraryPlaylistSource() {
  if (fWindow->fDlnaController)
    fWindow->fDlnaController->SetServerFieldVisible(false);
  fWindow->fLibraryManager->ContentView()->SetRadioMode(false);
  fWindow->fLibraryManager->ContentView()->SetFolderMode(false);
  fWindow->fLibraryManager->SetRadioFilterMode(false);
  fWindow->fLibraryManager->SetActivePaths({});
  if (fWindow->fMediaLibraryCache) {
    BMessage watchMsg(MSG_WATCH_FOLDER);
    watchMsg.AddString("path", "");
    BMessenger(fWindow->fMediaLibraryCache).SendMessage(&watchMsg);
  }
  fWindow->UpdateFilteredViews();
}

void PlaylistSelectionController::ShowRegularPlaylistSource(const BString &name) {
  if (fWindow->fDlnaController)
    fWindow->fDlnaController->SetServerFieldVisible(false);
  fWindow->fLibraryManager->ContentView()->SetRadioMode(false);
  fWindow->fLibraryManager->ContentView()->SetFolderMode(false);
  fWindow->fLibraryManager->SetRadioFilterMode(false);
  std::vector<BString> paths = fWindow->fPlaylistLibrary->LoadPlaylist(name);
  // Tracks put in the playlist before playlists joined them to the library
  // show only a file name: add them now.
  if (fWindow->fLibraryController)
    fWindow->fLibraryController->AddPlaylistTracks(paths);
  fWindow->fLibraryManager->SetActivePaths(paths);
  if (fWindow->fMediaLibraryCache) {
    BMessage watchMsg(MSG_WATCH_FOLDER);
    watchMsg.AddString("path", "");
    BMessenger(fWindow->fMediaLibraryCache).SendMessage(&watchMsg);
  }
  fWindow->UpdateFilteredViews();
}

void PlaylistSelectionController::ShowFolderPlaylistSource(const BString &name) {
  if (fWindow->fDlnaController)
    fWindow->fDlnaController->SetServerFieldVisible(false);
  fWindow->fLibraryManager->ContentView()->SetRadioMode(false);
  fWindow->fLibraryManager->ContentView()->SetFolderMode(true);
  fWindow->fLibraryManager->SetRadioFilterMode(false);

  BString folderPath = fWindow->fPlaylistLibrary->FolderPathForName(name);

  if (fWindow->fLibraryManager) {
    fWindow->fLibraryManager->SetActiveFolderPath(folderPath);
  }

  if (fWindow->fMediaLibraryCache) {
    // 1) Trigger asynchronous catch-up scan
    BMessage scanMsg(MSG_RESCAN);
    scanMsg.AddString("path", folderPath);
    BMessenger(fWindow->fMediaLibraryCache).SendMessage(&scanMsg);

    // 2) Trigger node monitoring for this folder
    BMessage watchMsg(MSG_WATCH_FOLDER);
    watchMsg.AddString("path", folderPath);
    BMessenger(fWindow->fMediaLibraryCache).SendMessage(&watchMsg);
  }
  fWindow->UpdateFilteredViews();
}

void PlaylistSelectionController::ResetAndHideFilters() {
  if (fWindow->fLibraryManager) {
    if (fWindow->fLibraryManager->GenreView())
      fWindow->fLibraryManager->GenreView()->DeselectAll();
    if (fWindow->fLibraryManager->ArtistView())
      fWindow->fLibraryManager->ArtistView()->DeselectAll();
    if (fWindow->fLibraryManager->AlbumView())
      fWindow->fLibraryManager->AlbumView()->DeselectAll();
  }
  fWindow->UpdateFilteredViews();
  if (fWindow->fFilterGroup && fWindow->fIsFilterGroupVisible) {
    fWindow->fIsFilterGroupVisible = false;
    fWindow->fFilterGroup->Hide();
  }
}
