/*
 * Copyright 2005-2026, Daniel Weber.
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#ifndef BETON_PLAYLIST_EDIT_CONTROLLER_H
#define BETON_PLAYLIST_EDIT_CONTROLLER_H

#include <Message.h>
#include <String.h>
#include <vector>

class MainWindow;
class BFilePanel;
class MediaTableView;
class BRow;

class PlaylistEditController {
public:
  PlaylistEditController(MainWindow *window);
  ~PlaylistEditController();

  void ShowSmartPlaylistGenerator();
  void GenerateSmartPlaylist(BMessage *msg);
  void MovePlaylistItem(BMessage *msg);
  void ReorderPlaylist(BMessage *msg);
  void HandlePlaylistDrop(BMessage *msg);
  void AddSelectedItemsToPlaylist(BMessage *msg);
  void PromptNewPlaylist(BMessage *msg);
  void SaveSelectedPlaylist();
  void SelectPlaylistFolder();
  void CreatePlaylistFromPrompt(BMessage *msg);
  void RenamePlaylistFromPrompt(BMessage *msg);
  void ReplyWithPlaylistNames(BMessage *msg);
  void HandlePlaylistFolderSelected(BMessage *msg);
  void DeleteSelectedPlaylistItems();
  void MoveSelectedItemsToTrash();
  void SelectMoveToFolder();
  void HandleMoveToFolderSelected(BMessage *msg);
  void MoveSelectedItemsTo(const entry_ref *targetDirRef);
  void RestorePlaylistPaths(BMessage *msg);
  void CreatePlaylistWithPaths(BMessage *msg);
  void DeletePlaylistByName(BMessage *msg);
  void ResolveRefRecursively(const struct entry_ref &ref,
                             std::vector<BString> &outPaths);

private:
  // Drop the given rows from the content view directly, without re-reading the
  // whole folder from disk. Used in Folder mode after files are moved/trashed
  // out of the folder (there is no playlist file to rewrite, and a full
  // disk re-read is slow).
  void _RemoveRowsFromView(MediaTableView *cv, const std::vector<BRow *> &rows);

  MainWindow *fWindow;
  BMessage fPendingPlaylistFiles;
};

#endif // BETON_PLAYLIST_EDIT_CONTROLLER_H
