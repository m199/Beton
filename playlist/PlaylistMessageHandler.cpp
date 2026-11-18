/*
 * Copyright 2005-2026, Daniel Weber.
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "PlaylistMessageHandler.h"

#include "CDRipWindow.h"
#include "MainWindow.h"
#include "LibraryController.h"
#include "PlaylistEditController.h"
#include "PlaybackTransportController.h"
#include "Messages.h"

#include <Alert.h>
#include <Catalog.h>
#include <Drivers.h>
#include <Message.h>
#include <OS.h>
#include <fs_info.h>
#include <fs_volume.h>

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "PlaylistMessageHandler"

namespace {

/** How long to keep trying while Beton itself lets go of the disc. */
const bigtime_t kEjectBusyTimeout = 10000000;

/**
 * Unmounts the disc mounted at @p mountPoint and ejects it, the way the
 * mount server does. Runs on its own thread: unmounting waits for the drive.
 */
void EjectDisc(const BString &mountPoint, const BString &label,
               const BMessenger &window) {
  const dev_t root = dev_for_path("/");
  const dev_t device = dev_for_path(mountPoint.String());
  fs_info info;
  if (device < 0 || device == root || fs_stat_dev(device, &info) != 0) {
    window.SendMessage(MSG_CD_RESCAN); // Already gone.
    return;
  }

  // Stopping playback releases the disc a moment later, so retry while busy.
  const bigtime_t deadline = system_time() + kEjectBusyTimeout;
  status_t status;
  while ((status = fs_unmount_volume(mountPoint.String(), 0)) == B_BUSY &&
         system_time() < deadline)
    snooze(250000);
  if (status != B_OK) {
    BMessage failed(MSG_CD_EJECT_FAILED);
    BString text;
    if (status == B_BUSY)
      text.SetToFormat(B_TRANSLATE("\"%s\" can't be ejected because it is "
                                   "in use."),
                       label.String());
    else
      text.SetToFormat(B_TRANSLATE("\"%s\" can't be ejected: %s"),
                       label.String(), strerror(status));
    failed.AddString("text", text);
    window.SendMessage(&failed);
    return;
  }

  // The mount server removes mount points it made in "/"; do the same.
  if (dev_for_path(mountPoint.String()) == root)
    rmdir(mountPoint.String());

  int fd = open(info.device_name, O_RDONLY);
  if (fd >= 0) {
    ioctl(fd, B_EJECT_DEVICE, nullptr, 0);
    close(fd);
  }
  window.SendMessage(MSG_CD_RESCAN);
}

} // namespace

PlaylistMessageHandler::PlaylistMessageHandler(MainWindow *window)
    : fWindow(window) {}

bool PlaylistMessageHandler::HandleMessage(BMessage *msg) {
  if (!fWindow || !msg)
    return false;

  switch (msg->what) {
  case MSG_CD_RIP: {
    // One rip at a time: the drive can only read one disc quickly anyway.
    if (fWindow->fCDRipWindow.IsValid()) {
      fWindow->fCDRipWindow.SendMessage(msg);
      break;
    }
    BString path, label;
    if (msg->FindString("path", &path) != B_OK || path.IsEmpty())
      break;
    msg->FindString("label", &label);
    CDRipWindow *ripWindow = new CDRipWindow(path, label, BMessenger(fWindow));
    fWindow->fCDRipWindow = BMessenger(ripWindow);
    ripWindow->Show();
    break;
  }

  case MSG_CD_EJECT: {
    BString path, label;
    if (msg->FindString("path", &path) != B_OK || path.IsEmpty())
      break;
    msg->FindString("label", &label);
    // An open rip window decides first: it may be ripping this disc.
    if (!msg->GetBool("rip_window_done", false) &&
        fWindow->fCDRipWindow.IsValid() &&
        fWindow->fCDRipWindow.SendMessage(msg) == B_OK)
      break;

    PlaybackTransportController *transport =
        fWindow->fPlaybackTransportController;
    BString prefix(path);
    prefix << '/';
    if (transport != nullptr && transport->NowPlayingPath().StartsWith(prefix))
      transport->StopPlayback();

    BMessenger target(fWindow);
    fWindow->LaunchThread("cd_eject", [path, label, target]() {
      EjectDisc(path, label, target);
    });
    break;
  }

  case MSG_CD_EJECT_FAILED: {
    BString text;
    msg->FindString("text", &text);
    BAlert *alert = new BAlert(B_TRANSLATE("Eject Disc"), text.String(),
                               B_TRANSLATE("OK"), nullptr, nullptr,
                               B_WIDTH_AS_USUAL, B_WARNING_ALERT);
    alert->Go(nullptr);
    break;
  }

  case MSG_NEW_SMART_PLAYLIST: {
    fWindow->fPlaylistEditController->ShowSmartPlaylistGenerator();
    break;
  }

  case MSG_GENERATE_PLAYLIST: {
    fWindow->fPlaylistEditController->GenerateSmartPlaylist(msg);
    break;
  }

  case MSG_MOVE_UP:
  case MSG_MOVE_DOWN: {
    fWindow->fPlaylistEditController->MovePlaylistItem(msg);
    break;
  }

  case MSG_REORDER_PLAYLIST: {
    fWindow->fPlaylistEditController->ReorderPlaylist(msg);
    break;
  }

  case MSG_PLAYLIST_ORDER_CHANGED: {
    fWindow->SaveSettings();
    break;
  }

  case MSG_MOVE_TO_TRASH: {
    fWindow->fPlaylistEditController->MoveSelectedItemsToTrash();
    break;
  }

  case MSG_MOVE_TO: {
    fWindow->fPlaylistEditController->SelectMoveToFolder();
    break;
  }

  case MSG_MOVE_TO_FOLDER_SELECTED: {
    fWindow->fPlaylistEditController->HandleMoveToFolderSelected(msg);
    break;
  }

  case MSG_RESTORE_PLAYLIST_PATHS: {
    fWindow->fPlaylistEditController->RestorePlaylistPaths(msg);
    break;
  }

  case MSG_CREATE_PLAYLIST_WITH_PATHS: {
    fWindow->fPlaylistEditController->CreatePlaylistWithPaths(msg);
    break;
  }

  case MSG_DELETE_PLAYLIST_BY_NAME: {
    fWindow->fPlaylistEditController->DeletePlaylistByName(msg);
    break;
  }

  case B_SIMPLE_DATA: {
    // Files dropped straight onto the track list while it shows the library
    // (drops onto the sidebar arrive with a "playlist" field).
    if (!msg->HasString("playlist") && msg->HasRef("refs") &&
        fWindow->fIsLibraryMode && !fWindow->fIsCDMode &&
        !fWindow->fIsRadioMode && !fWindow->fIsDlnaMode &&
        !fWindow->fIsFolderMode) {
      fWindow->fLibraryController->AddRefsToLibrary(msg);
      break;
    }
    // Onto the open sidebar folder's track list: into that folder on disk.
    // Not Beton's own rows, which a slip of the mouse would rearrange on disk.
    if (!msg->HasString("playlist") && msg->HasRef("refs") &&
        fWindow->fIsFolderMode && !msg->HasBool("beton_tracks")) {
      BMessage drop(*msg);
      drop.what = MSG_DROP_INTO_FOLDER;
      drop.AddString("folder", fWindow->fPlaylistLibrary->FolderPathForName(
                                   fWindow->fCurrentPlaylistName));
      fWindow->fLibraryController->DropIntoFolder(&drop);
      break;
    }
    fWindow->fPlaylistEditController->HandlePlaylistDrop(msg);
    break;
  }

  case MSG_ADD_TO_PLAYLIST: {
    fWindow->fPlaylistEditController->AddSelectedItemsToPlaylist(msg);
    break;
  }

  case MSG_NEW_PLAYLIST: {
    fWindow->fPlaylistEditController->PromptNewPlaylist(msg);
    break;
  }

  case MSG_SAVE_PLAYLIST_SELECTION: {
    fWindow->fPlaylistEditController->SaveSelectedPlaylist();
    break;
  }

  case MSG_SET_PLAYLIST_FOLDER: {
    fWindow->fPlaylistEditController->SelectPlaylistFolder();
    break;
  }

  case MSG_PLAYLIST_CREATED: {
    fWindow->fPlaylistEditController->CreatePlaylistFromPrompt(msg);
    break;
  }

  case MSG_NAME_PROMPT_RENAME: {
    fWindow->fPlaylistEditController->RenamePlaylistFromPrompt(msg);
    break;
  }

  case MSG_LIST_PLAYLIST: {
    fWindow->fPlaylistEditController->ReplyWithPlaylistNames(msg);
    break;
  }

  case MSG_PLAYLIST_FOLDER_SELECTED: {
    fWindow->fPlaylistEditController->HandlePlaylistFolderSelected(msg);
    break;
  }

  default:
    return false;
  }

  return true;
}
