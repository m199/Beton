/*
 * Copyright 2005-2026, Daniel Weber.
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "PlaylistSidebarView.h"
#include "Debug.h"
#include "LibraryController.h"
#include "Messages.h"
#include "PlaylistNameDialog.h"
#include "PlaylistLibrary.h"
#include <Alert.h>
#include <Application.h>
#include <Catalog.h>
#include <Directory.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <IconUtils.h>
#include <InterfaceDefs.h>
#include <MenuItem.h>
#include <Path.h>
#include <Resources.h>
#include <Roster.h>
#include <String.h>
#include <TextControl.h>
#include <MimeType.h>
#include <Node.h>
#include <NodeInfo.h>
#include <VolumeRoster.h>
#include <Volume.h>
#include <fs_info.h>
#include <algorithm>
#include <cinttypes>
#include <map>
#include <stdio.h>
#include <string.h>

#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "PlaylistSidebarView"

static constexpr int32 ICON_LIB_ID = 1001;
static constexpr int32 ICON_PL_ID = 1002;
static constexpr int32 ICON_RADIO_ID = 1003;
static constexpr int32 ICON_DLNA_ID = 1004;
static constexpr int32 ICON_FOLDER_ID = 1006;
static constexpr int32 ICON_CD_ID = 1007;

/*!	Icon of the volume mounted at \a cdPath, or NULL if it has none.

	Deliberately scoped to the one disc: with several drives mounted, falling
	back to "any cdda volume" would hand every sidebar row the same icon.
*/
static BBitmap *LoadSystemCDIcon(float size, const char *cdPath) {
  BVolume vol;
  bool found = false;

  if (cdPath && cdPath[0] != '\0') {
    // Not via an entry_ref: for a mount point that names the parent's volume.
    dev_t device = dev_for_path(cdPath);
    if (device >= 0) {
      vol.SetTo(device);
      if (vol.InitCheck() == B_OK) {
        found = true;
      }
    }
  }

  if (found) {
    uint8 *data = nullptr;
    size_t len = 0;
    type_code type = 0;
    if (vol.GetIcon(&data, &len, &type) == B_OK && data != nullptr && len > 0) {
      BRect r(0, 0, size - 1, size - 1);
      auto *bmp = new BBitmap(r, 0, B_RGBA32);
      if (BIconUtils::GetVectorIcon(data, len, bmp) == B_OK) {
        delete[] data;
        return bmp;
      }
      delete[] data;
      delete bmp;
    }

    BRect r(0, 0, size - 1, size - 1);
    auto *bmp = new BBitmap(r, 0, B_RGBA32);
    if (vol.GetIcon(bmp, B_MINI_ICON) == B_OK) {
      return bmp;
    }
    delete bmp;
  }

  return nullptr;
}

static BBitmap *LoadVectorIconFromResourceID(int32 id, float size) {
  if (!be_app || !be_app->AppResources())
    return nullptr;

  size_t len = 0;
  const void *data =
      be_app->AppResources()->LoadResource(B_VECTOR_ICON_TYPE, id, &len);
  if (!data || len == 0) {
    DEBUG_PRINT("Icon ID %ld not found\n", (long)id);
    return nullptr;
  }

  BRect r(0, 0, size - 1, size - 1);
  auto *bmp = new BBitmap(r, 0, B_RGBA32);
  if (BIconUtils::GetVectorIcon(static_cast<const uint8 *>(data), len, bmp) !=
      B_OK) {
    delete bmp;
    DEBUG_PRINT("Icon ID %ld: decoding failed\n",
            (long)id);
    return nullptr;
  }
  return bmp;
}

static float BaselineForRow(BView *v, const BRect &rowRect) {
  font_height fh;
  v->GetFontHeight(&fh);
  const float textH = ceilf(fh.ascent + fh.descent + fh.leading);
  return rowRect.top + floorf((rowRect.Height() - textH) / 2.0f) + fh.ascent;
}

PlaylistSidebarView::PlaylistSidebarView(const char *name, BMessenger target, PlaylistLibrary *manager)
    : SingleColumnListView(name), fManager(manager), fTarget(target), fContextMenu(nullptr) {

  SetExtraBottomPadding(LineHeight());
  AddItem("Library", false, PlaylistItemKind::Library);

  fContextMenu = new BPopUpMenu("PlaylistMenu");
  fContextMenu->AddItem(
      new BMenuItem(B_TRANSLATE("Delete"), new BMessage(MSG_DELETE_PLAYLIST)));
  fContextMenu->AddItem(
      new BMenuItem(B_TRANSLATE("Rename"), new BMessage(MSG_RENAME_PLAYLIST)));
  fContextMenu->SetTargetForItems(this);
}

PlaylistSidebarView::~PlaylistSidebarView() {
  delete fContextMenu;
  for (auto &entry : fIconCdByPath)
    delete entry.icon;
}

void PlaylistSidebarView::SelectionChanged(int32 index) {
  if (index >= 0) {
    BMessage msg(MSG_PLAYLIST_SELECTION);
    msg.AddInt32("index", index);
    msg.AddString("name", ItemAt(index));
    if ((size_t)index < fRows.size())
      msg.AddInt32("kind", (int32)fRows[index].kind);
    msg.AddString("path", PathAt(index));

    fTarget.SendMessage(&msg);

    DEBUG_PRINT("SelectionChanged → %ld (%s)\n", (long)index,
                ItemAt(index).String());
  }
}

void PlaylistSidebarView::MessageReceived(BMessage *msg) {
  switch (msg->what) {
  case B_SIMPLE_DATA: {
    int32 sourceIndex;
    if (msg->FindInt32("playlist_index", &sourceIndex) == B_OK) {

      if (fDropLineIndex >= 0) {
        _ReorderItem(sourceIndex, fDropLineIndex);

        BMessage orderChanged(MSG_PLAYLIST_ORDER_CHANGED);
        fTarget.SendMessage(&orderChanged);
      }

      fDragIndex = -1;
      fDropLineIndex = -1;
      fIsDragging = false;
      Invalidate();
      break;
    }

    if (!msg->HasRef("refs"))
      break;

    bool allRefsAreDirectories = true;
    bool hasDirectoryRef = false;
    entry_ref droppedRef;
    for (int32 i = 0; msg->FindRef("refs", i, &droppedRef) == B_OK; ++i) {
      BEntry entry(&droppedRef, true);
      if (entry.InitCheck() != B_OK || !entry.Exists() ||
          !entry.IsDirectory()) {
        allRefsAreDirectories = false;
        break;
      }
      hasDirectoryRef = true;
    }

    int32 dropIndex = (int32)(fLastDropPoint.y / LineHeight());
    if (dropIndex < 0 || dropIndex >= CountItems()) {
      if (hasDirectoryRef && allRefsAreDirectories && fManager) {
        int32 lastAdded = -1;
        for (int32 i = 0; msg->FindRef("refs", i, &droppedRef) == B_OK; ++i) {
          BEntry entry(&droppedRef, true);
          BPath path;
          if (entry.GetPath(&path) == B_OK)
            lastAdded = fManager->AddFolderSource(path.Path());
        }
        if (lastAdded >= 0) {
          Select(lastAdded);
          SelectionChanged(lastAdded);
          BMessage changed(MSG_PLAYLIST_ORDER_CHANGED);
          fTarget.SendMessage(&changed);
        }
      } else {
        BMessage newMsg(MSG_NEW_PLAYLIST);
        newMsg.AddMessage("files", msg);
        fTarget.SendMessage(&newMsg);
      }
    } else if (KindAt(dropIndex) == PlaylistItemKind::Library) {
      // Onto "Library": the files (and folders' contents) join the library;
      // the folders don't become sidebar folders.
      BMessage add(MSG_ADD_TO_LIBRARY_REFS);
      entry_ref ref;
      for (int32 i = 0; msg->FindRef("refs", i, &ref) == B_OK; ++i)
        add.AddRef("refs", &ref);
      fTarget.SendMessage(&add);
    } else if (KindAt(dropIndex) == PlaylistItemKind::Folder) {
      // Onto a sidebar folder: into that folder on disk, as in Tracker.
      BMessage drop(MSG_DROP_INTO_FOLDER);
      drop.AddString("folder", fManager ? fManager->FolderPathForName(
                                              ItemAt(dropIndex))
                                        : PathAt(dropIndex));
      entry_ref ref;
      for (int32 i = 0; msg->FindRef("refs", i, &ref) == B_OK; ++i)
        drop.AddRef("refs", &ref);
      fTarget.SendMessage(&drop);
    } else {
      if (!IsWritableAt(dropIndex)) {
        if (hasDirectoryRef && allRefsAreDirectories && fManager) {
          int32 lastAdded = -1;
          for (int32 i = 0; msg->FindRef("refs", i, &droppedRef) == B_OK;
               ++i) {
            BEntry entry(&droppedRef, true);
            BPath path;
            if (entry.GetPath(&path) == B_OK)
              lastAdded = fManager->AddFolderSource(path.Path());
          }
          if (lastAdded >= 0) {
            Select(lastAdded);
            SelectionChanged(lastAdded);
            BMessage changed(MSG_PLAYLIST_ORDER_CHANGED);
            fTarget.SendMessage(&changed);
          }
          SetHoverIndex(-1);
          break;
        }
        DEBUG_PRINT("Drop on non-writable playlist -> "
                    "ignored (idx=%ld)\n",
                    (long)dropIndex);
        SetHoverIndex(-1);
        break;
      }

      BString playlistName = ItemAt(dropIndex);
      BMessage dropOnPlaylistMsg(B_SIMPLE_DATA);
      dropOnPlaylistMsg.AddString("playlist", playlistName);
      dropOnPlaylistMsg.AddMessage("files", msg);
      fTarget.SendMessage(&dropOnPlaylistMsg);
    }

    SetHoverIndex(-1);
    break;
  }

  case MSG_RENAME_PLAYLIST: {
    int32 index = CurrentSelection();
    if (index > 0 && index < CountItems() && IsWritableAt(index)) {
      BString oldName = ItemAt(index);
      PlaylistNameDialog *prompt = new PlaylistNameDialog(BMessenger(Window()));
      prompt->SetInitialName(oldName);
      prompt->SetMessageWhat(MSG_NAME_PROMPT_RENAME);
      prompt->Show();
      prompt->SetTitle(oldName.String());
    }
    break;
  }

  case MSG_DELETE_PLAYLIST:
    DEBUG_PRINT("MSG_DELETE_PLAYLIST received\n");
    RemoveSelectedPlaylist();
    break;

  case MSG_CD_RIP:
  case MSG_CD_EJECT:
  case MSG_PASTE_INTO_FOLDER:
    fTarget.SendMessage(msg);
    break;

  case MSG_OPEN_IN_TRACKER: {
    BString path;
    entry_ref ref;
    if (msg->FindString("path", &path) != B_OK || path.IsEmpty() ||
        get_ref_for_path(path.String(), &ref) != B_OK ||
        !BEntry(&ref, true).IsDirectory()) {
      BString text;
      text.SetToFormat(B_TRANSLATE("The folder \"%s\" can't be found."),
                       path.String());
      (new BAlert(B_TRANSLATE("Open in Tracker"), text.String(),
                  B_TRANSLATE("OK"), nullptr, nullptr, B_WIDTH_AS_USUAL,
                  B_WARNING_ALERT))
          ->Go(nullptr);
      break;
    }
    // Tracker is the preferred app for folders.
    status_t st = be_roster->Launch(&ref);
    if (st != B_OK && st != B_ALREADY_RUNNING)
      DEBUG_PRINT("Open in Tracker failed for %s: %s\n", path.String(),
                  strerror(st));
    break;
  }

  default:
    SingleColumnListView::MessageReceived(msg);
  }
}

int32 PlaylistSidebarView::AddItem(const char *title, bool writable) {
  return AddItem(title, writable, PlaylistItemKind::Playlist);
}

int32 PlaylistSidebarView::AddItem(const char *title, bool writable,
                                PlaylistItemKind kind) {
  return AddItem(title, "", writable, kind);
}

int32 PlaylistSidebarView::AddItem(const char *title, const char *path,
                                bool writable, PlaylistItemKind kind) {
  SingleColumnListView::AddItem(title, path);
  fRows.push_back({BString(title), writable, kind});
  Invalidate();
  return CountItems() - 1;
}

int32 PlaylistSidebarView::FindIndexByName(const BString &name) const {
  for (int32 i = 0; i < CountItems(); ++i) {
    if (ItemAt(i) == name)
      return i;
  }
  return -1;
}

void PlaylistSidebarView::MouseDown(BPoint where) {
  SetHoverIndex(-1);
  MakeFocus(true);

  int32 index = (int32)(where.y / LineHeight());
  if (index < 0 || index >= CountItems())
    index = -1;

  uint32 buttons;
  GetMouse(&where, &buttons);
  if ((buttons & B_SECONDARY_MOUSE_BUTTON) != 0 && index >= 0) {
    Select(index);
    ConvertToScreen(&where);

    BString clickedName = ItemAt(index);
    delete fContextMenu;
    fContextMenu = new BPopUpMenu("PlaylistMenu");

    if (IsWritableAt(index)) {
      fContextMenu->AddItem(new BMenuItem(B_TRANSLATE("Rename"),
                                          new BMessage(MSG_RENAME_PLAYLIST)));
      fContextMenu->AddItem(new BMenuItem(B_TRANSLATE("Delete"),
                                          new BMessage(MSG_DELETE_PLAYLIST)));
    } else if ((size_t)index < fRows.size() &&
               fRows[index].kind == PlaylistItemKind::Folder) {
      const BString folderPath =
          fManager ? fManager->FolderPathForName(clickedName) : PathAt(index);
      BMessage *open = new BMessage(MSG_OPEN_IN_TRACKER);
      open->AddString("path", folderPath);
      fContextMenu->AddItem(
          new BMenuItem(B_TRANSLATE("Open in Tracker"), open));
      BMessage *paste = new BMessage(MSG_PASTE_INTO_FOLDER);
      paste->AddString("folder", folderPath);
      BMenuItem *pasteItem =
          new BMenuItem(B_TRANSLATE("Paste into Folder"), paste);
      pasteItem->SetEnabled(LibraryController::ClipboardHasFiles());
      fContextMenu->AddItem(pasteItem);
      fContextMenu->AddSeparatorItem();
      fContextMenu->AddItem(new BMenuItem(B_TRANSLATE("Remove"),
                                          new BMessage(MSG_DELETE_PLAYLIST)));
    } else if (KindAt(index) == PlaylistItemKind::CD) {
      BMessage *rip = new BMessage(MSG_CD_RIP);
      rip->AddString("path", PathAt(index));
      rip->AddString("label", ItemAt(index));
      fContextMenu->AddItem(
          new BMenuItem(B_TRANSLATE("Rip CD" B_UTF8_ELLIPSIS), rip));
      BMessage *eject = new BMessage(MSG_CD_EJECT);
      eject->AddString("path", PathAt(index));
      eject->AddString("label", ItemAt(index));
      fContextMenu->AddItem(new BMenuItem(B_TRANSLATE("Eject Disc"), eject));
    }

    if (fContextMenu->CountItems() > 0) {
      fContextMenu->SetTargetForItems(this);
      BMenuItem *chosen =
          fContextMenu->Go(where, false, false, BRect(where, where), false);
      if (chosen) {
        DEBUG_PRINT("Menu selected: %s\n", chosen->Label());
        BMessage *m = chosen->Message();
        if (m)
          MessageReceived(m);
      }
    }
    return;
  }

  if (index >= _FirstPlaylistIndex() && (size_t)index < fRows.size() &&
      fRows[index].kind == PlaylistItemKind::Playlist) {
    fDragStartPoint = where;
    fDragIndex = index;
  } else {
    fDragIndex = -1;
  }

  SingleColumnListView::MouseDown(where);
}

void PlaylistSidebarView::MouseMoved(BPoint point, uint32 transit,
                                  const BMessage *dragMsg) {

  if (!fIsDragging && fDragIndex >= 0) {
    uint32 buttons;
    GetMouse(NULL, &buttons);

    if (buttons & B_PRIMARY_MOUSE_BUTTON) {

      float dx = point.x - fDragStartPoint.x;
      float dy = point.y - fDragStartPoint.y;
      float distance = sqrtf(dx * dx + dy * dy);

      if (distance > 5.0f) {
        fIsDragging = true;

        BMessage dragMsg(B_SIMPLE_DATA);
        dragMsg.AddInt32("playlist_index", fDragIndex);

        float rowTop = fDragIndex * LineHeight();
        BRect dragRect(0, rowTop, Bounds().Width(), rowTop + LineHeight() - 1);
        DragMessage(&dragMsg, dragRect, this);
        return;
      }
    } else {
      fDragIndex = -1;
      fIsDragging = false;
    }
  }

  if (dragMsg && dragMsg->what == B_SIMPLE_DATA) {
    int32 sourceIndex;
    if (dragMsg->FindInt32("playlist_index", &sourceIndex) == B_OK) {
      float rowH = LineHeight();
      int32 targetRow = (int32)((point.y + rowH / 2.0f) / rowH);
      targetRow = std::max(_FirstPlaylistIndex(),
                           std::min(targetRow, CountItems()));

      fDropLineIndex = targetRow;
      return;
    }

    if (dragMsg->HasRef("refs")) {
      fLastDropPoint = point;
      int32 idx = HitIndex(point);
      if (idx >= 0 &&
          (IsWritableAt(idx) || KindAt(idx) == PlaylistItemKind::Folder))
        SetHoverIndex(idx);
      else
        SetHoverIndex(-1);
    }
  } else {
    SetHoverIndex(-1);
    if (fDropLineIndex >= 0) {
      fDropLineIndex = -1;
    }
  }
  SingleColumnListView::MouseMoved(point, transit, dragMsg);
}

void PlaylistSidebarView::RenameItem(const BString &oldName,
                                  const BString &newName) {
  for (auto &item : fItems) {
    if (item.text == oldName) {
      item.text = newName;
      break;
    }
  }
  for (auto &r : fRows) {
    if (r.label == oldName) {
      r.label = newName;
      break;
    }
  }
  Invalidate();
}

void PlaylistSidebarView::AddFileToPlaylist(int32 index, const entry_ref &ref) {
  if (index < 0 || index >= CountItems())
    return;
  if (!IsWritableAt(index))
    return;

  BString playlistName = ItemAt(index);
  BPath path(&ref);
  if (fManager) {
    fManager->AddItemToPlaylist(playlistName, path.Path());
  }
  DEBUG_PRINT("File '%s' saved to playlist '%s'\n",
              path.Path(), playlistName.String());
}

void PlaylistSidebarView::RemoveSelectedPlaylist() {
  int32 index = CurrentSelection();
  if (index > 0 && index < CountItems()) {
    PlaylistItemKind kind = KindAt(index);
    if (!IsWritableAt(index) && kind != PlaylistItemKind::Folder)
      return;
    BString name = ItemAt(index);
    if (fManager) {
      if (kind == PlaylistItemKind::Folder)
        fManager->RemoveFolderSource(name);
      else
        fManager->DeletePlaylist(name);
    }
    SingleColumnListView::RemoveItemAt(index);
    if ((size_t)index < fRows.size())
      fRows.erase(fRows.begin() + index);
    fCurrentSelection = -1;
    UpdateScrollbars();
    Invalidate();
    BMessage changed(MSG_PLAYLIST_ORDER_CHANGED);
    fTarget.SendMessage(&changed);
    DEBUG_PRINT("Playlist source '%s' deleted\n", name.String());
  }
}

bool PlaylistSidebarView::RemovePlaylistAt(int32 index) {
  if (index < 0 || index >= CountItems())
    return false;
  SingleColumnListView::RemoveItemAt(index);
  if ((size_t)index < fRows.size())
    fRows.erase(fRows.begin() + index);
  Invalidate();
  UpdateScrollbars();
  return true;
}

int32 PlaylistSidebarView::HitIndex(BPoint p) const {
  if (p.y < 0)
    return -1;
  int32 idx = (int32)(p.y / LineHeight());
  return (idx >= 0 && idx < CountItems()) ? idx : -1;
}

void PlaylistSidebarView::SetHoverIndex(int32 idx) {
  if (idx == fHoverIndex)
    return;
  int32 old = fHoverIndex;
  fHoverIndex = idx;
  BRect bounds = Bounds();
  if (old >= 0 && old < CountItems()) {
    BRect r(bounds.left, old * LineHeight(), bounds.right,
            (old + 1) * LineHeight() - 1);
    Invalidate(r);
  }
  if (fHoverIndex >= 0 && fHoverIndex < CountItems()) {
    BRect r(bounds.left, fHoverIndex * LineHeight(), bounds.right,
            (fHoverIndex + 1) * LineHeight() - 1);
    Invalidate(r);
  }
}

void PlaylistSidebarView::_EnsureIconsLoaded() const {
  if (!fIconLibrary) {
    float rowH = const_cast<PlaylistSidebarView *>(this)->LineHeight();
    fIconSize = rowH * 0.7f;
    fIconLibrary = LoadVectorIconFromResourceID(ICON_LIB_ID, fIconSize);
  }
  // Generic fallback only - a mounted disc's own icon comes from _CdIconFor().
  if (!fIconCd) {
    fIconCd = LoadVectorIconFromResourceID(ICON_CD_ID, fIconSize);
    if (!fIconCd)
      fIconCd = fIconLibrary;
  }
  if (!fIconPlaylist)
    fIconPlaylist = LoadVectorIconFromResourceID(ICON_PL_ID, fIconSize);
  if (!fIconFolder) {
    fIconFolder = LoadVectorIconFromResourceID(ICON_FOLDER_ID, fIconSize);
    if (!fIconFolder)
      fIconFolder = fIconPlaylist;
  }
  if (!fIconRadio) {
    fIconRadio = LoadVectorIconFromResourceID(ICON_RADIO_ID, fIconSize);
    if (!fIconRadio)
      fIconRadio = fIconPlaylist;
  }
  if (!fIconDlna) {
    fIconDlna = LoadVectorIconFromResourceID(ICON_DLNA_ID, fIconSize);
    if (!fIconDlna)
      fIconDlna = fIconRadio;
  }
}

BBitmap *PlaylistSidebarView::_CdIconFor(const BString &path) const {
  if (path.IsEmpty())
    return fIconCd;

  for (const auto &entry : fIconCdByPath) {
    if (entry.path == path)
      return entry.icon ? entry.icon : fIconCd;
  }

  // First time we've drawn this disc: ask its volume for an icon. A null
  // result is cached too, so a disc without one doesn't re-query every frame.
  BBitmap *icon = LoadSystemCDIcon(fIconSize, path.String());
  fIconCdByPath.push_back(CdIconEntry{path, icon});
  return icon ? icon : fIconCd;
}

void PlaylistSidebarView::_PruneCdIcons(const std::vector<CDItemInfo> &mountedCDs) {
  for (size_t i = fIconCdByPath.size(); i-- > 0;) {
    bool stillMounted = false;
    for (const auto &cd : mountedCDs) {
      if (cd.path == fIconCdByPath[i].path) {
        stillMounted = true;
        break;
      }
    }
    if (!stillMounted) {
      delete fIconCdByPath[i].icon;
      fIconCdByPath.erase(fIconCdByPath.begin() + i);
    }
  }
}

BBitmap *PlaylistSidebarView::_IconFor(PlaylistItemKind kind,
                                       const BString &path) const {
  _EnsureIconsLoaded();
  switch (kind) {
  case PlaylistItemKind::Library:
    return fIconLibrary;
  case PlaylistItemKind::CD:
    return _CdIconFor(path);
  case PlaylistItemKind::Playlist:
    return fIconPlaylist;
  case PlaylistItemKind::Folder:
    return fIconFolder;
  case PlaylistItemKind::Radio:
    return fIconRadio;
  case PlaylistItemKind::DLNA:
    return fIconDlna;
  }
  return nullptr;
}

void PlaylistSidebarView::RenameCDPath(const BString &oldPath,
                                       const BString &newPath) {
  for (int32 i = 0; i < CountItems(); ++i) {
    if (KindAt(i) == PlaylistItemKind::CD && PathAt(i) == oldPath &&
        (size_t)i < fItems.size()) {
      fItems[i].path = newPath;
      return;
    }
  }
}

void PlaylistSidebarView::SyncCDItems(const std::vector<CDItemInfo> &mountedCDs) {
  // Each disc gets its own icon, resolved lazily on first draw by _CdIconFor();
  // drop the cached icons of discs that have gone away.
  _PruneCdIcons(mountedCDs);

  bool selectedCDRemoved = false;

  for (int32 i = CountItems() - 1; i >= 0; --i) {
    if (KindAt(i) == PlaylistItemKind::CD) {
      BString existingPath = PathAt(i);
      bool stillMounted = false;
      for (const auto &cd : mountedCDs) {
        if (cd.path == existingPath) {
          stillMounted = true;
          break;
        }
      }
      if (!stillMounted) {
        if (fCurrentSelection == i)
          selectedCDRemoved = true;

        SingleColumnListView::RemoveItemAt(i);
        if ((size_t)i < fRows.size())
          fRows.erase(fRows.begin() + i);

        if (fCurrentSelection > i)
          fCurrentSelection--;
      }
    }
  }

  for (const auto &cd : mountedCDs) {
    int32 existingIndex = -1;
    for (int32 i = 0; i < CountItems(); ++i) {
      if (KindAt(i) == PlaylistItemKind::CD && PathAt(i) == cd.path) {
        existingIndex = i;
        break;
      }
    }

    if (existingIndex >= 0) {
      if ((size_t)existingIndex < fItems.size()) {
        fItems[existingIndex].text = cd.label;
        fItems[existingIndex].path = cd.path;
      }
      if ((size_t)existingIndex < fRows.size()) {
        fRows[existingIndex].label = cd.label;
        fRows[existingIndex].writable = false;
        fRows[existingIndex].kind = PlaylistItemKind::CD;
      }
    } else {
      int32 insertIndex = 1;
      for (int32 i = 0; i < CountItems(); ++i) {
        if (KindAt(i) == PlaylistItemKind::CD) {
          insertIndex = i + 1;
        }
      }
      if (insertIndex > CountItems())
        insertIndex = CountItems();

      fItems.insert(fItems.begin() + insertIndex, SimpleItem{cd.label, cd.path, false});
      fRows.insert(fRows.begin() + insertIndex, PlaylistRow{cd.label, false, PlaylistItemKind::CD});
    }
  }

  Invalidate();
  UpdateScrollbars();

  if (selectedCDRemoved) {
    SelectByName("Library");
  }
}

int32 PlaylistSidebarView::SetCDItem(const char *title, const char *path) {
  SyncCDItems({{BString(title), BString(path)}});
  return FindIndexByName(title);
}

void PlaylistSidebarView::RemoveCDItem() {
  SyncCDItems({});
}

bool PlaylistSidebarView::IsWritableAt(int32 index) const {
  if (index < 0 || index >= CountItems())
    return false;
  if ((size_t)index >= fRows.size())
    return false;
  return fRows[index].writable;
}

PlaylistItemKind PlaylistSidebarView::KindAt(int32 index) const {
  if (index < 0 || index >= CountItems())
    return PlaylistItemKind::Playlist;
  if ((size_t)index >= fRows.size())
    return PlaylistItemKind::Playlist;
  return fRows[index].kind;
}

void PlaylistSidebarView::SetIsUnwritableAt(int32 index, bool v) {
  if (index < 0 || index >= CountItems())
    return;
  if ((size_t)index >= fRows.size())
    return;
  fRows[index].writable = !v;
  Invalidate();
}

void PlaylistSidebarView::SetIsUnwritableByName(const BString &name, bool v) {
  for (int32 i = 0; i < CountItems(); ++i) {
    if (ItemAt(i) == name) {
      SetIsUnwritableAt(i, v);
      break;
    }
  }
}

void PlaylistSidebarView::Draw(BRect updateRect) {
  const float rowH = LineHeight();
  BRect bounds = Bounds();

  const rgb_color base = ui_color(B_LIST_BACKGROUND_COLOR);
  const float avg = (base.red + base.green + base.blue) / 3.0f;
  const bool isDark = (avg < 128.0f);
  const rgb_color stripe = tint_color(base, isDark ? 0.90f : 1.05f);

  const rgb_color selBG = fUseCustomColor
                              ? fSelectionColor
                              : ui_color(B_LIST_SELECTED_BACKGROUND_COLOR);
  const rgb_color textColor = ui_color(B_LIST_ITEM_TEXT_COLOR);
  const rgb_color selTextCol = fUseCustomColor
                                   ? fSelectionTextColor
                                   : ui_color(B_LIST_SELECTED_ITEM_TEXT_COLOR);

  const int32 first = std::max<int32>(0, (int32)floorf(updateRect.top / rowH));
  const int32 last = std::min<int32>(CountItems() - 1,
                                     (int32)floorf(updateRect.bottom / rowH));

  for (int32 i = first; i <= last; ++i) {
    BRect rowRect(bounds.left, i * rowH, bounds.right, (i + 1) * rowH - 1);
    if (!rowRect.Intersects(updateRect))
      continue;

    const bool selected =
        ((size_t)i < fItems.size() ? fItems[i].selected
                                   : (i == fCurrentSelection));

    if (selected) {

      SetHighColor(selBG);
      FillRect(rowRect);
      SetHighColor((rgb_color){152, 152, 152, 255});
      StrokeRect(rowRect);
    } else {

      SetHighColor(((i & 1) == 0) ? base : stripe);
      FillRect(rowRect);
    }

    const float iconX = rowRect.left + fIconPadX;
    const float iconY =
        rowRect.top + floorf((rowRect.Height() + 1 - fIconSize) / 2.0f);

    if ((size_t)i < fRows.size()) {
      if (BBitmap *icon = _IconFor(fRows[i].kind, PathAt(i))) {

        SetDrawingMode(B_OP_ALPHA);
        SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
        DrawBitmap(icon, BPoint(iconX, iconY));
        SetDrawingMode(B_OP_COPY);
      }
    }

    const float textLeft = iconX + fIconSize + fIconPadX;
    const float baseline = BaselineForRow(this, rowRect);
    SetHighColor(selected ? selTextCol : textColor);
    MovePenTo(textLeft, baseline);
    const BString &label =
        (size_t)i < fRows.size() ? fRows[i].label : fItems[i].text;
    DrawString(label.String());
  }

  {
    int32 nextRow =
        std::max<int32>(CountItems(), (int32)floorf(updateRect.top / rowH));
    float y = nextRow * rowH;
    while (y <= updateRect.bottom) {
      SetHighColor(((nextRow & 1) == 0) ? base : stripe);
      FillRect(BRect(bounds.left, y, bounds.right,
                     std::min(y + rowH - 1.0f, updateRect.bottom)));
      ++nextRow;
      y += rowH;
    }
  }

  if (fHoverIndex >= 0 && fHoverIndex < CountItems() &&
      fHoverIndex != fCurrentSelection && IsWritableAt(fHoverIndex)) {

    BRect rowRect(bounds.left, fHoverIndex * rowH, bounds.right,
                  (fHoverIndex + 1) * rowH - 1);
    if (rowRect.Intersects(updateRect)) {
      rgb_color c = ui_color(B_CONTROL_HIGHLIGHT_COLOR);
      c.alpha = 60;
      SetDrawingMode(B_OP_ALPHA);
      SetHighColor(c);
      FillRect(rowRect);

      SetDrawingMode(B_OP_COPY);
      rgb_color frame = tint_color(c, B_DARKEN_1_TINT);
      frame.alpha = 255;
      SetHighColor(frame);
      StrokeRect(rowRect);
    }
  }

  // Drop line indicator intentionally removed.
}

int32 PlaylistSidebarView::_FirstPlaylistIndex() const {
  for (int32 i = 0; i < (int32)fRows.size(); ++i) {
    if (fRows[i].kind == PlaylistItemKind::Playlist)
      return i;
  }
  return CountItems();
}

void PlaylistSidebarView::_ReorderItem(int32 from, int32 to) {
  int32 firstPlaylistIndex = _FirstPlaylistIndex();
  if (from < firstPlaylistIndex || from >= CountItems() ||
      (size_t)from >= fRows.size() ||
      fRows[from].kind != PlaylistItemKind::Playlist ||
      to < firstPlaylistIndex || to > CountItems() || from == to)
    return;

  int32 targetIndex = to;
  if (to > from)
    targetIndex--;
  if (targetIndex < firstPlaylistIndex)
    return;

  SimpleItem movedItem = fItems[from];
  PlaylistRow movedRow = fRows[from];

  fItems.erase(fItems.begin() + from);
  fRows.erase(fRows.begin() + from);

  fItems.insert(fItems.begin() + targetIndex, movedItem);
  fRows.insert(fRows.begin() + targetIndex, movedRow);

  Select(targetIndex);

  Invalidate();
  ScrollToSelection();

  DEBUG_PRINT("Reordered: %ld -> %ld\n", (long)from,
              (long)targetIndex);
}

std::vector<BString> PlaylistSidebarView::GetPlaylistOrder() const {
  std::vector<BString> order;
  for (int32 i = 0; i < CountItems(); ++i) {
    order.push_back(ItemAt(i));
  }
  return order;
}

void PlaylistSidebarView::SetPlaylistOrder(const std::vector<BString> &order) {
  DEBUG_PRINT("SetPlaylistOrder called with %zu items\n",
              order.size());

  std::map<BString, int32> targetPositions;
  for (size_t i = 0; i < order.size(); ++i) {
    targetPositions[order[i]] = i;
    DEBUG_PRINT("  Order[%zu] = '%s'\n", i,
                order[i].String());
  }

  std::vector<SimpleItem> newItems;
  std::vector<PlaylistRow> newRows;

  int32 libIndex = FindIndexByName("Library");
  if (libIndex >= 0 && libIndex < (int32)fItems.size()) {
    newItems.push_back(fItems[libIndex]);
    newRows.push_back(fRows[libIndex]);
  }

  for (size_t i = 0; i < fRows.size(); ++i) {
    if (fRows[i].kind == PlaylistItemKind::CD) {
      newItems.push_back(fItems[i]);
      newRows.push_back(fRows[i]);
    }
  }

  int32 radioIndex = FindIndexByName("Radio");
  if (radioIndex >= 0 && radioIndex < (int32)fItems.size()) {
    newItems.push_back(fItems[radioIndex]);
    newRows.push_back(fRows[radioIndex]);
  }

  int32 dlnaIndex = FindIndexByName("DLNA");
  if (dlnaIndex >= 0 && dlnaIndex < (int32)fItems.size()) {
    newItems.push_back(fItems[dlnaIndex]);
    newRows.push_back(fRows[dlnaIndex]);
  }

  for (const auto &name : order) {
    if (name == "Library" || name == "Radio" || name == "DLNA")
      continue;
    int32 currentIndex = FindIndexByName(name);
    if (currentIndex >= 0 && currentIndex < (int32)fItems.size() &&
        fRows[currentIndex].kind != PlaylistItemKind::CD) {
      newItems.push_back(fItems[currentIndex]);
      newRows.push_back(fRows[currentIndex]);
      DEBUG_PRINT("  Found '%s' at index %ld\n",
                  name.String(), (long)currentIndex);
    }
  }

  for (size_t i = 0; i < fItems.size(); ++i) {
    const BString &name = fItems[i].text;
    if (name == "Library" || name == "Radio" || name == "DLNA" ||
        fRows[i].kind == PlaylistItemKind::CD)
      continue;
    if (targetPositions.find(name) == targetPositions.end()) {
      newItems.push_back(fItems[i]);
      newRows.push_back(fRows[i]);
      DEBUG_PRINT("  Appending '%s' (not in saved order)\n",
                  name.String());
    }
  }

  fItems = newItems;
  fRows = newRows;

  Invalidate();
  UpdateScrollbars();

  DEBUG_PRINT(
      "SetPlaylistOrder complete, now have %zu items\n",
      fItems.size());
}

int32 PlaylistSidebarView::SelectByName(const BString &name) {
  int32 index = FindIndexByName(name);
  if (index >= 0) {
    Select(index);
    SelectionChanged(index);
    ScrollToSelection();
  }
  return index;
}
