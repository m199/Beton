/*
 * Copyright 2005-2026, Daniel Weber.
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#ifndef BETON_PLAYLIST_SIDEBAR_VIEW_H
#define BETON_PLAYLIST_SIDEBAR_VIEW_H

#include "SingleColumnListView.h"
#include <Bitmap.h>
#include <Entry.h>
#include <Messenger.h>
#include <PopUpMenu.h>
#include <String.h>
#include <vector>

class PlaylistLibrary;

// Logical source types shown in the playlist sidebar.
enum class PlaylistItemKind { Library, CD, Playlist, Folder, Radio, DLNA };

struct PlaylistRow {
  /** @name Data */
  ///@{
  BString label;
  bool writable;
  PlaylistItemKind kind;
  ///@}
};

struct CDItemInfo {
  BString label;
  BString path;
};

class PlaylistSidebarView : public SingleColumnListView {
public:
  PlaylistSidebarView(const char *name, BMessenger target,
                      PlaylistLibrary *manager);
  virtual ~PlaylistSidebarView();

  void SelectionChanged(int32 index) override;
  /** Points the CD row for @p oldPath at @p newPath (the disc was renamed). */
  void RenameCDPath(const BString &oldPath, const BString &newPath);
  void MessageReceived(BMessage *msg) override;
  void MouseDown(BPoint where) override;
  void MouseMoved(BPoint point, uint32 transit,
                  const BMessage *dragMsg) override;
  void Draw(BRect updateRect) override;

  /** @name Modification Logic */
  ///@{

  void AddFileToPlaylist(int32 index, const entry_ref &ref);
  void RemoveSelectedPlaylist();
  void RenameItem(const BString &oldName, const BString &newName);

  bool IsWritableAt(int32 index) const;
  PlaylistItemKind KindAt(int32 index) const;
  void SetIsUnwritableAt(int32 index, bool v);
  void SetIsUnwritableByName(const BString &name, bool v);
  bool RemovePlaylistAt(int32 index);

  void SyncCDItems(const std::vector<CDItemInfo> &cds);
  int32 SetCDItem(const char *title, const char *path);
  void RemoveCDItem();

  int32 AddItem(const char *title, bool writable);
  int32 AddItem(const char *title, bool writable, PlaylistItemKind kind);
  int32 AddItem(const char *title, const char *path, bool writable = true,
                PlaylistItemKind kind = PlaylistItemKind::Playlist);

  std::vector<BString> GetPlaylistOrder() const;
  void SetPlaylistOrder(const std::vector<BString> &order);
  int32 SelectByName(const BString &name);
  ///@}
  /** @name Internal Helpers */
  ///@{
  int32 FindIndexByName(const BString &name) const;

private:
  int32 HitIndex(BPoint p) const;
  void SetHoverIndex(int32 idx);

  void _EnsureIconsLoaded() const;
  BBitmap *_IconFor(PlaylistItemKind kind, const BString &path) const;
  BBitmap *_CdIconFor(const BString &path) const;
  void _PruneCdIcons(const std::vector<CDItemInfo> &mountedCDs);

  int32 _FirstPlaylistIndex() const;
  void _ReorderItem(int32 from, int32 to);
  ///@}

  PlaylistLibrary *fManager;
  BMessenger fTarget;
  BPopUpMenu *fContextMenu;
  std::vector<PlaylistRow> fRows;
  int32 fHoverIndex{-1};
  BPoint fLastDropPoint;

  int32 fDragIndex{-1};
  int32 fDropLineIndex{-1};
  BPoint fDragStartPoint;
  bool fIsDragging{false};

  /// Per-disc volume icon, keyed by mount path. A null bitmap means the disc
  /// has no icon of its own and the generic fIconCd should be drawn instead.
  struct CdIconEntry {
    BString path;
    BBitmap *icon;
  };
  mutable std::vector<CdIconEntry> fIconCdByPath;

  mutable BBitmap *fIconLibrary = nullptr;
  mutable BBitmap *fIconCd = nullptr;
  mutable BBitmap *fIconPlaylist = nullptr;
  mutable BBitmap *fIconFolder = nullptr;
  mutable BBitmap *fIconRadio = nullptr;
  mutable BBitmap *fIconDlna = nullptr;

  mutable float fIconSize = 16.0f;
  float fIconPadX = 6.0f;
  float fIconPadY = 2.0f;
};

#endif // BETON_PLAYLIST_SIDEBAR_VIEW_H
