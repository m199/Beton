/*
 * Copyright 2026, David Beckman, indispensablelabs@gmail.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

/*
 * Files and folders dropped onto a sidebar folder, or onto its open track
 * list, go into that folder the way Tracker does it: moved on the same disk,
 * copied from another one. The folder watch then adds them to the library.
 */

#include "LibraryController.h"

#include "Debug.h"
#include "MainWindow.h"
#include "Messages.h"
#include "UndoManager.h"

#include <Alert.h>
#include <Catalog.h>
#include <Clipboard.h>
#include <Messenger.h>
#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <Node.h>
#include <fs_attr.h>
#include <Path.h>
#include <SymLink.h>
#include <Volume.h>

#include <atomic>
#include <memory>
#include <string.h>
#include <vector>

#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "LibraryFolderDrop"

namespace {

/** A file is copied under this suffix and renamed when complete, so the
 *  folder watch never reads a half-written track. */
const char *kPartSuffix = ".beton-part";
const size_t kCopyBufferSize = 256 * 1024;

/** How an item reaches the folder: a drop (moved on the same disk, copied
 *  from another), or Paste after Tracker's Copy or Cut. */
enum DropMode { kModeDrop = 0, kModeCopy = 1, kModeCut = 2 };

// Tracker's clipboard (src/kits/tracker/FSClipboard.cpp, Commands.h,
// tracker_private.h); private, so not includable.
const uint32 kTrackerCopySelectionTo = 'Tcsl';
const uint32 kTrackerMoveSelectionTo = 'Tmsl';
const uint32 kTrackerDelete = 'Tdel';
const uint32 kTrackerClipboardChanges = 'TCch';
const int32 kTrackerClipboardNode = 'TCNR';
const char *kTrackerSignature = "application/x-vnd.Be-TRAK";
struct TrackerClipboardNode {
  node_ref node;
  uint32 moveMode;
};

struct DropItem {
  BString from;
  BString to;
  bool copy;      ///< Copied, not moved.
  bool trashFrom; ///< Cut from another disk: the original goes to the Trash.
  bool conflict;
  bool isDir;
};

bool IsUnder(const BString &path, const BString &dir) {
  BString prefix(dir);
  if (!prefix.EndsWith("/"))
    prefix << "/";
  return path == dir || path.StartsWith(prefix);
}

/** @p leaf in @p dir, or "name 1.ext", "name 2.ext"... if that is taken. */
BString UniquePath(const char *dir, const char *leaf, bool isDir) {
  BPath path(dir, leaf);
  if (!BEntry(path.Path()).Exists())
    return path.Path();
  BString name(leaf), base(leaf), ext;
  const int32 dot = name.FindLast('.');
  if (!isDir && dot > 0) {
    name.CopyInto(base, 0, dot);
    name.CopyInto(ext, dot, name.Length() - dot);
  }
  for (int32 n = 1;; n++) {
    BString candidate;
    candidate << base << " " << n << ext;
    path.SetTo(dir, candidate.String());
    if (!BEntry(path.Path()).Exists())
      return path.Path();
  }
}

/** A free name in the Trash of the disk that holds @p path. */
BString TrashPathFor(const BString &path) {
  BEntry entry(path.String());
  entry_ref ref;
  if (entry.GetRef(&ref) != B_OK)
    return BString();
  BVolume volume(ref.device);
  BPath trash;
  if (volume.InitCheck() != B_OK ||
      find_directory(B_TRASH_DIRECTORY, &trash, true, &volume) != B_OK)
    return BString();
  return UniquePath(trash.Path(), ref.name, entry.IsDirectory());
}

void CopyAttributes(BNode &from, BNode &to) {
  from.RewindAttrs();
  char name[B_ATTR_NAME_LENGTH];
  std::vector<char> data;
  while (from.GetNextAttrName(name) == B_OK) {
    attr_info info;
    if (from.GetAttrInfo(name, &info) != B_OK)
      continue;
    data.resize((size_t)info.size);
    if (from.ReadAttr(name, info.type, 0, data.data(), info.size) ==
        info.size)
      to.WriteAttr(name, info.type, 0, data.data(), info.size); // FAT refuses
  }
}

status_t CopyEntry(const BEntry &from, BDirectory &toDir,
                   const char *leaf, const std::atomic<bool> &stop);

status_t CopyFile(const BEntry &from, BDirectory &toDir,
                  const char *leaf, const std::atomic<bool> &stop) {
  BFile in(&from, B_READ_ONLY);
  status_t status = in.InitCheck();
  if (status != B_OK)
    return status;

  BString partName;
  partName << "." << leaf << kPartSuffix;
  BFile out(&toDir, partName.String(),
            B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
  if ((status = out.InitCheck()) != B_OK)
    return status;

  std::unique_ptr<char[]> buffer(new char[kCopyBufferSize]);
  for (;;) {
    if (stop.load()) {
      status = B_CANCELED;
      break;
    }
    const ssize_t got = in.Read(buffer.get(), kCopyBufferSize);
    if (got < 0) {
      status = (status_t)got;
      break;
    }
    if (got == 0)
      break;
    const ssize_t written = out.Write(buffer.get(), (size_t)got);
    if (written != got) {
      status = written < 0 ? (status_t)written : B_DEVICE_FULL;
      break;
    }
  }

  if (status == B_OK) {
    CopyAttributes(in, out);
    mode_t perms;
    if (in.GetPermissions(&perms) == B_OK)
      out.SetPermissions(perms);
    time_t modified;
    if (in.GetModificationTime(&modified) == B_OK)
      out.SetModificationTime(modified);
  }
  out.Unset();

  BEntry part(&toDir, partName.String());
  if (status == B_OK)
    status = part.Rename(leaf, false);
  if (status != B_OK)
    part.Remove();
  return status;
}

status_t CopyDirectory(const BEntry &from, BDirectory &toDir,
                       const char *leaf, const std::atomic<bool> &stop) {
  BDirectory created;
  status_t status = toDir.CreateDirectory(leaf, &created);
  if (status != B_OK)
    return status;
  BNode fromNode(&from);
  if (fromNode.InitCheck() == B_OK)
    CopyAttributes(fromNode, created);

  BDirectory source(&from);
  if ((status = source.InitCheck()) != B_OK)
    return status;
  BEntry child;
  while (source.GetNextEntry(&child, false) == B_OK) {
    char name[B_FILE_NAME_LENGTH];
    child.GetName(name);
    if ((status = CopyEntry(child, created, name, stop)) != B_OK)
      return status;
  }
  return B_OK;
}

status_t CopyEntry(const BEntry &from, BDirectory &toDir,
                   const char *leaf, const std::atomic<bool> &stop) {
  if (stop.load())
    return B_CANCELED;
  if (from.IsSymLink()) {
    BSymLink link(&from);
    char target[B_PATH_NAME_LENGTH];
    const ssize_t length = link.ReadLink(target, sizeof(target));
    if (length < 0)
      return (status_t)length;
    return toDir.CreateSymLink(leaf, target, nullptr);
  }
  if (from.IsDirectory())
    return CopyDirectory(from, toDir, leaf, stop);
  return CopyFile(from, toDir, leaf, stop);
}

} // namespace

void LibraryController::DropIntoFolder(const BMessage *msg) {
  BString folder;
  if (msg->FindString("folder", &folder) != B_OK || folder.IsEmpty())
    return;
  BDirectory dir(folder.String());
  node_ref dirRef;
  if (dir.InitCheck() != B_OK || dir.GetNodeRef(&dirRef) != B_OK) {
    fWindow->UpdateStatus(B_TRANSLATE("The folder can't be found."));
    return;
  }
  const char *folderName = BPath(folder.String()).Leaf();

  std::vector<DropItem> items;
  int32 conflicts = 0;
  entry_ref ref;
  for (int32 i = 0; msg->FindRef("refs", i, &ref) == B_OK; i++) {
    BEntry entry(&ref); // A link is moved or copied as a link.
    BPath path(&entry);
    if (!entry.Exists() || path.InitCheck() != B_OK)
      continue;
    const int32 mode = msg->GetInt32("mode", i, kModeDrop);
    const bool isDir = entry.IsDirectory();
    const bool sameDisk = ref.device == dirRef.device;
    // A folder can't go into itself.
    if (isDir && IsUnder(folder, path.Path()))
      continue;

    DropItem item;
    item.from = path.Path();
    item.to = BPath(folder.String(), ref.name).Path();
    item.copy = mode == kModeCopy || !sameDisk;
    item.trashFrom = mode == kModeCut && !sameDisk;
    item.isDir = isDir;
    item.conflict = BEntry(item.to.String()).Exists();

    // Already in this folder: a copy becomes a numbered duplicate, as in
    // Tracker; anything else stays where it is.
    BEntry parent;
    node_ref parentRef;
    if (entry.GetParent(&parent) == B_OK &&
        parent.GetNodeRef(&parentRef) == B_OK && parentRef == dirRef) {
      if (mode != kModeCopy)
        continue;
      item.to = UniquePath(folder.String(), ref.name, isDir);
      item.conflict = false;
    }
    if (item.conflict)
      conflicts++;
    items.push_back(item);
  }
  if (items.empty())
    return;

  // Ask once for every name that is taken. A folder is never replaced,
  // only kept beside the other one.
  enum { kSkip, kKeepBoth, kReplace } choice = kKeepBoth;
  if (conflicts > 0) {
    BString text;
    if (conflicts == 1) {
      for (const DropItem &item : items)
        if (item.conflict)
          text.SetToFormat(B_TRANSLATE("\"%s\" is already in \"%s\"."),
                           BPath(item.to.String()).Leaf(), folderName);
    } else {
      text.SetToFormat(
          B_TRANSLATE("%ld items with the same names are already in \"%s\"."),
          (long)conflicts, folderName);
    }
    text << "\n\n"
         << B_TRANSLATE("Replace moves the files that are there to the "
                        "Trash. Keep both gives the new ones a number.");
    BAlert *alert = new BAlert(B_TRANSLATE("Name in use"), text.String(),
                               B_TRANSLATE("Skip"), B_TRANSLATE("Keep both"),
                               B_TRANSLATE("Replace"), B_WIDTH_AS_USUAL,
                               B_OFFSET_SPACING, B_WARNING_ALERT);
    alert->SetShortcut(0, B_ESCAPE);
    const int32 button = alert->Go();
    choice = button == 0 ? kSkip : button == 2 ? kReplace : kKeepBoth;
  }

  std::vector<BMessage> undoFirst, undoLast, redoFirst, redoLast;
  auto recordMove = [](const BString &from, const BString &to,
                       std::vector<BMessage> &undo,
                       std::vector<BMessage> &redo) {
    BMessage u(MSG_FILE_MOVE);
    u.AddString("from", to);
    u.AddString("to", from);
    undo.push_back(u);
    BMessage r(MSG_FILE_MOVE);
    r.AddString("from", from);
    r.AddString("to", to);
    redo.push_back(r);
  };
  auto move = [this](const BString &from, const BString &to) {
    BMessage m(MSG_FILE_MOVE);
    m.AddString("from", from);
    m.AddString("to", to);
    m.AddBool("undo_replay", true);
    HandleFileMove(&m);
    return BEntry(to.String()).Exists() && !BEntry(from.String()).Exists();
  };

  std::vector<DropItem> copies;
  int32 moved = 0;
  for (DropItem &item : items) {
    if (item.conflict) {
      if (choice == kSkip)
        continue;
      const bool replace = choice == kReplace && !item.isDir &&
                           !BEntry(item.to.String()).IsDirectory();
      if (replace) {
        // The file that was there goes to the Trash first; Undo restores it
        // after the new one has moved out again.
        const BString trash = TrashPathFor(item.to);
        if (trash.IsEmpty() || !move(item.to, trash))
          continue;
        recordMove(item.to, trash, undoLast, redoFirst);
      } else {
        item.to = UniquePath(folder.String(), BPath(item.to.String()).Leaf(),
                             item.isDir);
      }
    }
    if (item.copy) {
      copies.push_back(item);
      continue;
    }
    if (move(item.from, item.to)) {
      recordMove(item.from, item.to, undoFirst, redoLast);
      moved++;
    }
  }

  if (fWindow->fUndoManager && moved > 0) {
    std::vector<BMessage> undo(undoFirst), redo(redoFirst);
    undo.insert(undo.end(), undoLast.begin(), undoLast.end());
    redo.insert(redo.end(), redoLast.begin(), redoLast.end());
    fWindow->fUndoManager->RecordAction(undo, redo);
  } else if (fWindow->fUndoManager && !undoLast.empty() && copies.empty()) {
    fWindow->fUndoManager->RecordAction(undoLast, redoFirst);
  }

  if (copies.empty()) {
    if (moved > 0) {
      BString status;
      status.SetToFormat(B_TRANSLATE("Moved %ld items to %s."), (long)moved,
                         folderName);
      fWindow->UpdateStatus(status);
    }
    return;
  }

  BString status;
  status.SetToFormat(B_TRANSLATE("Copying %ld items to %s" B_UTF8_ELLIPSIS),
                     (long)copies.size(), folderName);
  fWindow->UpdateStatus(status, true);

  // A file replaced by a copy was trashed above; that is undone with the
  // copy, once the copy is done.
  // (When something was moved too, the trashing is already in that step.)
  BMessage done(MSG_FOLDER_COPY_DONE);
  done.AddString("folder", folder);
  if (moved == 0) {
    for (const BMessage &m : undoLast)
      done.AddMessage("undo_after", &m);
    for (const BMessage &m : redoFirst)
      done.AddMessage("redo_before", &m);
  }

  BMessenger window(fWindow);
  const std::atomic<bool> *stop = &fWindow->fShuttingDown;
  fWindow->LaunchThread("folder_copy", [copies, done, window, stop]() mutable {
    int32 failed = 0;
    status_t lastError = B_OK;
    for (const DropItem &item : copies) {
      BEntry from(item.from.String());
      BPath to(item.to.String());
      BPath toDir;
      to.GetParent(&toDir);
      BDirectory dir(toDir.Path());
      const status_t status = CopyEntry(from, dir, to.Leaf(), *stop);
      if (status == B_OK) {
        done.AddString("to", item.to);
        done.AddString("from", item.from);
        done.AddBool("trash_from", item.trashFrom);
      } else {
        failed++;
        lastError = status;
        DEBUG_PRINT("copy %s -> %s failed: %s\n", item.from.String(),
                    item.to.String(), strerror(status));
        if (status == B_CANCELED)
          break;
      }
    }
    done.AddInt32("failed", failed);
    done.AddInt32("error", lastError);
    window.SendMessage(&done);
  });
}

void LibraryController::FolderCopyDone(const BMessage *msg) {
  BString folder;
  msg->FindString("folder", &folder);
  const char *folderName = BPath(folder.String()).Leaf();

  // Undo puts each copy in the Trash, then restores what it replaced; Redo
  // does the opposite. Trash paths are picked now so Redo can find them.
  std::vector<BMessage> undo, redo;
  BMessage m;
  for (int32 i = 0; msg->FindMessage("redo_before", i, &m) == B_OK; i++)
    redo.push_back(m);
  int32 copied = 0;
  BString to, from;
  std::vector<BMessage> restoreOriginals;
  for (int32 i = 0; msg->FindString("to", i, &to) == B_OK; i++) {
    copied++;
    const BString trash = TrashPathFor(to);
    if (!trash.IsEmpty()) {
      // Undo: the copy goes to the Trash.
      BMessage u(MSG_FILE_MOVE);
      u.AddString("from", to);
      u.AddString("to", trash);
      undo.push_back(u);
      BMessage r(MSG_FILE_MOVE);
      r.AddString("from", trash);
      r.AddString("to", to);
      redo.push_back(r);
    }

    // Cut from another disk: the original goes to the Trash (Tracker deletes
    // it; the Trash keeps Undo possible).
    if (!msg->GetBool("trash_from", i, false) ||
        msg->FindString("from", i, &from) != B_OK)
      continue;
    const BString fromTrash = TrashPathFor(from);
    if (fromTrash.IsEmpty())
      continue;
    BMessage m(MSG_FILE_MOVE);
    m.AddString("from", from);
    m.AddString("to", fromTrash);
    m.AddBool("undo_replay", true);
    HandleFileMove(&m);
    if (!BEntry(fromTrash.String()).Exists())
      continue;
    BMessage u(MSG_FILE_MOVE);
    u.AddString("from", fromTrash);
    u.AddString("to", from);
    restoreOriginals.push_back(u);
    BMessage r(MSG_FILE_MOVE);
    r.AddString("from", from);
    r.AddString("to", fromTrash);
    redo.push_back(r);
  }
  undo.insert(undo.end(), restoreOriginals.begin(), restoreOriginals.end());
  for (int32 i = 0; msg->FindMessage("undo_after", i, &m) == B_OK; i++)
    undo.push_back(m);
  if (fWindow->fUndoManager && !undo.empty())
    fWindow->fUndoManager->RecordAction(undo, redo);

  const int32 failed = msg->GetInt32("failed", 0);
  BString status;
  if (failed == 0) {
    status.SetToFormat(B_TRANSLATE("Copied %ld items to %s."), (long)copied,
                       folderName);
  } else {
    status.SetToFormat(
        B_TRANSLATE("Copied %ld items to %s; %ld could not be copied (%s)."),
        (long)copied, folderName, (long)failed,
        strerror(msg->GetInt32("error", B_ERROR)));
  }
  fWindow->UpdateStatus(status);
}

namespace {

/**
 * Reads the files Tracker put on the clipboard: for each, a ref named
 * "r<device>_<node>" and its mode, Copy or Cut, named "m<device>_<node>".
 * Must be called with be_clipboard locked.
 */
void ReadTrackerClipboard(BMessage *clip, BMessage &refs,
                          std::vector<BString> *cutModeNames) {
  char *name;
  type_code type;
  int32 count;
  for (int32 i = 0;
       clip->GetInfo(B_REF_TYPE, i, &name, &type, &count) == B_OK; i++) {
    if (name[0] != 'r')
      continue;
    BString modeName(name);
    modeName.SetByteAt(0, 'm');
    int32 mode;
    entry_ref ref;
    if (clip->FindInt32(modeName.String(), &mode) != B_OK ||
        clip->FindRef(name, &ref) != B_OK || !BEntry(&ref).Exists())
      continue;
    if (mode != (int32)kTrackerCopySelectionTo &&
        mode != (int32)kTrackerMoveSelectionTo)
      continue;
    const bool cut = mode == (int32)kTrackerMoveSelectionTo;
    refs.AddRef("refs", &ref);
    refs.AddInt32("mode", cut ? kModeCut : kModeCopy);
    if (cut && cutModeNames != nullptr)
      cutModeNames->push_back(modeName);
  }
}

} // namespace

bool LibraryController::ClipboardHasFiles() {
  BMessage refs;
  if (!be_clipboard->Lock())
    return false;
  if (BMessage *clip = be_clipboard->Data())
    ReadTrackerClipboard(clip, refs, nullptr);
  be_clipboard->Unlock();
  return refs.HasRef("refs");
}

void LibraryController::PasteIntoFolder(const BString &folder) {
  BMessage drop(MSG_DROP_INTO_FOLDER);
  drop.AddString("folder", folder);
  if (!be_clipboard->Lock())
    return;
  BMessage *clip = be_clipboard->Data();
  std::vector<BString> cutModeNames;
  if (clip != nullptr)
    ReadTrackerClipboard(clip, drop, &cutModeNames);

  // As Tracker does after pasting: cut files become copied ones, so a second
  // Paste copies them, and Tracker stops showing their icons dimmed.
  if (clip != nullptr && !cutModeNames.empty()) {
    for (const BString &modeName : cutModeNames) {
      clip->ReplaceInt32(modeName.String(), (int32)kTrackerCopySelectionTo);
      BString refName(modeName);
      refName.SetByteAt(0, 'r');
      entry_ref ref;
      if (clip->FindRef(refName.String(), &ref) != B_OK)
        continue;
      BNode node(&ref);
      TrackerClipboardNode changed;
      if (node.GetNodeRef(&changed.node) != B_OK)
        continue;
      changed.moveMode = kTrackerDelete;
      BMessage update(kTrackerClipboardChanges);
      update.AddInt32("device", ref.device);
      update.AddInt64("directory", ref.directory);
      update.AddData("tcnode", kTrackerClipboardNode, &changed,
                     sizeof(changed));
      BMessenger(kTrackerSignature).SendMessage(&update);
    }
    be_clipboard->Commit();
  }
  be_clipboard->Unlock();

  if (!drop.HasRef("refs")) {
    fWindow->UpdateStatus(
        B_TRANSLATE("Nothing to paste: copy or cut files in Tracker first."));
    return;
  }
  DropIntoFolder(&drop);
}
