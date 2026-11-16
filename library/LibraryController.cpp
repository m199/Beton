/*
 * Copyright 2005-2026, Daniel Weber.
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "LibraryController.h"
#include "AudioPlaybackEngine.h"
#include "GainPrefetcher.h"
#include "GainStore.h"
#include "LoudnessAnalyzer.h"
#include "MainWindow.h"
#include "MetadataPropertiesWindow.h"
#include "LibraryBrowserController.h"
#include "MusicSourceManagerWindow.h"
#include "MediaLibraryCache.h"
#include "Debug.h"
#include "Messages.h"
#include "PlaybackQueueManager.h"
#include "PlaylistLibrary.h"
#include "PlaylistSidebarView.h"
#include "MediaTableView.h"
#include "StatusBarController.h"
#include "UndoManager.h"
#include <Alert.h>
#include <Catalog.h>
#include <Directory.h>
#include <NumberFormat.h>
#include <MessageRunner.h>
#include <Entry.h>
#include <File.h>
#include <FilePanel.h>
#include <FindDirectory.h>
#include <Messenger.h>
#include <Node.h>
#include <OS.h>
#include <Path.h>
#include <Roster.h>
#include <TextView.h>
#include <Volume.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "LibraryController"

LibraryController::LibraryController(MainWindow* window) : fWindow(window) {}
LibraryController::~LibraryController() {
  _StopScanStatusTimer();
  delete fAddPanel;
  // Its thread has ended: the window waits for launched threads first.
  delete fGainPrefetcher;
}

/**
 * @brief Opens the directory/source management window.
 */
void LibraryController::ShowDirectoryManager() {
  MusicSourceManagerWindow *win = new MusicSourceManagerWindow(fWindow->fMediaLibraryCache);
  win->Show();
}

// Tracker's restore-from-Trash attribute (kAttrOriginalPath in Haiku's
// src/kits/tracker/Attributes.h; private, so not includable).
static const char *kAttrOriginalPath = "_trk/original_path";

/**
 * @brief Returns true if @a dir is the Trash of the volume it lives on.
 */
static bool _IsTrashDirectory(const BDirectory &dir) {
  node_ref dirRef;
  if (dir.GetNodeRef(&dirRef) != B_OK)
    return false;

  BVolume volume(dirRef.device);
  BPath trashPath;
  if (volume.InitCheck() != B_OK ||
      find_directory(B_TRASH_DIRECTORY, &trashPath, false, &volume) != B_OK)
    return false;

  node_ref trashRef;
  BDirectory trash(trashPath.Path());
  return trash.GetNodeRef(&trashRef) == B_OK && trashRef == dirRef;
}

namespace {

bool IsInside(const BString &path, const std::vector<BString> &dirs) {
  for (const BString &dir : dirs) {
    BString prefix(dir);
    if (!prefix.EndsWith("/"))
      prefix << "/";
    if (path == dir || path.StartsWith(prefix))
      return true;
  }
  return false;
}

} // namespace

void LibraryController::ImportRippedFiles(BMessage *msg) {
  if (!fWindow->fMediaLibraryCache)
    return;

  // Read the files even if the library has them: a rip into a sidebar folder
  // is picked up as each file appears, before its loudness (and a WAV's tags
  // and attributes) are written. Without "Add tracks to library" only those
  // already picked up that way are read again.
  const bool add = msg->GetBool("add", true);
  BMessage scan(MSG_SCAN_FILES);
  scan.AddBool("refresh", true);
  if (!add)
    scan.AddBool("known_only", true);
  int32 queued = 0;
  BString path;
  for (int32 i = 0; msg->FindString("path", i, &path) == B_OK; i++) {
    scan.AddString("path", path);
    scan.AddInt32("playlist", -1); // Not from a playlist.
    queued++;
  }

  DEBUG_PRINT("CD rip: %s %ld ripped files\n",
              add ? "adding" : "refreshing", (long)queued);
  if (queued > 0)
    BMessenger(fWindow->fMediaLibraryCache).SendMessage(&scan);
}

std::vector<BString> LibraryController::_SidebarFolders() const {
  return fWindow->fPlaylistLibrary ? fWindow->fPlaylistLibrary->FolderSourcePaths()
                                   : std::vector<BString>();
}

void LibraryController::RemoveSelectedFromLibrary() {
  MediaTableView *cv = fWindow->fLibraryManager
                           ? fWindow->fLibraryManager->ContentView()
                           : nullptr;
  if (!cv || fWindow->fIsCDMode || fWindow->fIsRadioMode ||
      fWindow->fIsDlnaMode)
    return;

  // A sidebar folder's tracks mirror the folder: they leave the library when
  // their files leave the folder, not by hand.
  const std::vector<BString> folders = _SidebarFolders();
  std::vector<BString> paths;
  int32 inFolders = 0;
  for (BRow *row = cv->CurrentSelection(); row; row = cv->CurrentSelection(row)) {
    const MediaItem *mi = cv->ItemAt(cv->IndexOf(row));
    if (!mi || mi->path.IsEmpty())
      continue;
    if (IsInside(mi->path, folders))
      inFolders++;
    else
      paths.push_back(mi->path);
  }

  if (paths.empty()) {
    if (inFolders > 0)
      (new BAlert(B_TRANSLATE("Remove from Library"),
                  B_TRANSLATE("These tracks are in a sidebar folder, so they "
                              "stay in the library as long as their files are "
                              "in the folder. Move the files out of the folder "
                              "or to the Trash to remove them."),
                  B_TRANSLATE("OK"), nullptr, nullptr, B_WIDTH_AS_USUAL,
                  B_INFO_ALERT))
          ->Go(nullptr);
    return;
  }

  BString text;
  if (paths.size() == 1)
    text = B_TRANSLATE("Remove this track from the library?");
  else
    text.SetToFormat(B_TRANSLATE("Remove %ld tracks from the library?"),
                     (long)paths.size());
  text << "\n\n"
       << B_TRANSLATE("The files stay where they are. The tracks are also "
                      "taken out of your playlists.");
  if (inFolders > 0) {
    BString skipped;
    skipped.SetToFormat(B_TRANSLATE("%ld selected tracks are in sidebar "
                                    "folders and stay."),
                        (long)inFolders);
    text << "\n\n" << skipped;
  }
  BAlert *alert = new BAlert(B_TRANSLATE("Remove from Library"), text.String(),
                             B_TRANSLATE("Cancel"), B_TRANSLATE("Remove"),
                             nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
  alert->SetShortcut(0, B_ESCAPE);
  if (alert->Go() != 1)
    return;

  _RemoveFromLibrary(paths);
}

void LibraryController::RemoveMissingTracks() {
  // Missing tracks inside sidebar folders are on a disk that isn't mounted
  // (gone files there leave the library on their own): keep them.
  const std::vector<BString> folders = _SidebarFolders();
  std::vector<BString> paths;
  for (const MediaItem &item : fWindow->fAllItems)
    if (item.missing && !IsInside(item.path, folders))
      paths.push_back(item.path);

  if (paths.empty()) {
    (new BAlert(B_TRANSLATE("Remove Missing Tracks"),
                B_TRANSLATE("Every track in the library has its file."),
                B_TRANSLATE("OK"), nullptr, nullptr, B_WIDTH_AS_USUAL,
                B_INFO_ALERT))
        ->Go(nullptr);
    return;
  }

  BString text;
  if (paths.size() == 1)
    text = B_TRANSLATE("1 track's file can't be found. Remove it from the "
                       "library?");
  else
    text.SetToFormat(B_TRANSLATE("%ld tracks' files can't be found. Remove "
                                 "them from the library?"),
                     (long)paths.size());
  text << "\n\n"
       << B_TRANSLATE("They are also taken out of your playlists.");
  BAlert *alert = new BAlert(B_TRANSLATE("Remove Missing Tracks"),
                             text.String(), B_TRANSLATE("Cancel"),
                             B_TRANSLATE("Remove"), nullptr, B_WIDTH_AS_USUAL,
                             B_WARNING_ALERT);
  alert->SetShortcut(0, B_ESCAPE);
  if (alert->Go() != 1)
    return;

  _RemoveFromLibrary(paths);
}

void LibraryController::AddPlaylistTracks(const std::vector<BString> &paths) {
  // Before the cache has loaded every track would look new; the cache's own
  // load covers them.
  if (!fWindow->fMediaLibraryCache || !fWindow->fCacheLoaded)
    return;
  BMessage scan(MSG_SCAN_FILES);
  int32 queued = 0;
  for (const BString &path : paths) {
    if (path.IsEmpty() || path[0] != '/' ||
        fWindow->fPathIndex.count(path) > 0)
      continue;
    scan.AddString("path", path);
    scan.AddInt32("playlist", -1);
    queued++;
  }
  if (queued == 0)
    return;
  DEBUG_PRINT("Adding %ld playlist tracks to the library\n", (long)queued);
  BMessenger(fWindow->fMediaLibraryCache).SendMessage(&scan);
}

void LibraryController::RemovePathsFromLibrary(const BMessage *msg) {
  std::vector<BString> paths;
  BString path;
  for (int32 i = 0; msg->FindString("path", i, &path) == B_OK; i++)
    paths.push_back(path);
  if (!paths.empty())
    _RemoveFromLibrary(paths, !msg->HasBool("undo_replay"));
}

void LibraryController::RestoreToLibrary(const BMessage *msg) {
  if (!fWindow->fMediaLibraryCache)
    return;
  BMessage restore(*msg);
  restore.RemoveName("undo_replay");
  BMessenger(fWindow->fMediaLibraryCache).SendMessage(&restore);

  type_code type;
  int32 count = 0;
  msg->GetInfo("path", &type, &count);
  BString status;
  status.SetToFormat(B_TRANSLATE("Put %ld tracks back in the library."),
                     (long)count);
  fWindow->UpdateStatus(status, false);
}

void LibraryController::_RemoveFromLibrary(const std::vector<BString> &paths,
                                           bool recordUndo) {
  BMessage undo(MSG_LIBRARY_RESTORE);
  for (const BString &path : paths)
    undo.AddString("path", path);

  // Playlist tracks are library tracks: leaving one would bring the track
  // back at the next library rebuild.
  if (fWindow->fPlaylistLibrary) {
    std::set<BString> gone(paths.begin(), paths.end());
    BMessage names;
    fWindow->fPlaylistLibrary->GetPlaylistNames(names, true);
    BString name;
    for (int32 i = 0; names.FindString("name", i, &name) == B_OK; ++i) {
      std::vector<BString> entries =
          fWindow->fPlaylistLibrary->LoadPlaylist(name);
      const std::vector<BString> before = entries;
      entries.erase(std::remove_if(entries.begin(), entries.end(),
                                   [&gone](const BString &p) {
                                     return gone.count(p) > 0;
                                   }),
                    entries.end());
      if (entries.size() == before.size())
        continue;
      fWindow->fPlaylistLibrary->SavePlaylist(name, entries);

      // Undo puts the playlist back as it was, after the tracks are back.
      BMessage restore(MSG_RESTORE_PLAYLIST_PATHS);
      restore.AddString("playlist", name);
      for (const BString &p : before)
        restore.AddString("paths", p);
      undo.AddMessage("then", &restore);
    }
  }

  if (recordUndo && fWindow->fUndoManager) {
    BMessage redo(MSG_LIBRARY_REMOVE);
    for (const BString &path : paths)
      redo.AddString("path", path);
    fWindow->fUndoManager->RecordAction({undo}, {redo});
  }

  // The cache answers with MSG_MEDIA_ITEM_REMOVED for each track, which takes
  // it out of the window's list and the view.
  if (fWindow->fMediaLibraryCache) {
    BMessage remove(MSG_LIBRARY_REMOVE);
    for (const BString &path : paths)
      remove.AddString("path", path);
    BMessenger(fWindow->fMediaLibraryCache).SendMessage(&remove);
  }

  BString status;
  status.SetToFormat(B_TRANSLATE("Removed %ld tracks from the library."),
                     (long)paths.size());
  fWindow->UpdateStatus(status, false);
}

void LibraryController::ShowAddToLibraryPanel() {
  if (fAddPanel == nullptr) {
    fAddPanel = new BFilePanel(B_OPEN_PANEL, new BMessenger(fWindow), nullptr,
                               B_FILE_NODE | B_DIRECTORY_NODE, true,
                               new BMessage(MSG_ADD_TO_LIBRARY_REFS));
    fAddPanel->Window()->SetTitle(B_TRANSLATE("Beton: Add to Library"));
    fAddPanel->SetButtonLabel(B_DEFAULT_BUTTON, B_TRANSLATE("Add"));
  }
  fAddPanel->Show();
}

namespace {

struct AddToLibraryJob {
  std::vector<entry_ref> refs;
  BMessenger cache;
  BMessenger window;
};

/** Adds the audio files at or under @p ref to @p out. */
void CollectAudioFiles(const entry_ref &ref, std::set<BString> &out,
                       std::set<node_ref> &visitedDirs) {
  BEntry entry(&ref, true);
  BPath path;
  if (entry.InitCheck() != B_OK || entry.GetPath(&path) != B_OK)
    return;
  if (!entry.IsDirectory()) {
    if (MediaLibraryCache::IsAudioFile(path.Path()))
      out.insert(path.Path());
    return;
  }
  // Links are followed, so remember folders to stop at a loop.
  node_ref dirRef;
  if (entry.GetNodeRef(&dirRef) != B_OK || !visitedDirs.insert(dirRef).second)
    return;
  BDirectory dir(&entry);
  entry_ref child;
  while (dir.GetNextRef(&child) == B_OK) {
    if (child.name != nullptr && child.name[0] == '.')
      continue;
    CollectAudioFiles(child, out, visitedDirs);
  }
}

} // namespace

void LibraryController::AddRefsToLibrary(const BMessage *msg) {
  if (!fWindow->fMediaLibraryCache)
    return;
  auto *job = new AddToLibraryJob;
  entry_ref ref;
  for (int32 i = 0; msg->FindRef("refs", i, &ref) == B_OK; i++)
    job->refs.push_back(ref);
  if (job->refs.empty()) {
    delete job;
    return;
  }
  job->cache = BMessenger(fWindow->fMediaLibraryCache);
  job->window = BMessenger(fWindow);
  thread_id thread = spawn_thread(_AddToLibraryThread, "add to library",
                                  B_LOW_PRIORITY, job);
  if (thread < B_OK || resume_thread(thread) != B_OK) {
    delete job;
    return;
  }
  fWindow->UpdateStatus(B_TRANSLATE("Looking for music to add..."), false);
}

status_t LibraryController::_AddToLibraryThread(void *data) {
  std::unique_ptr<AddToLibraryJob> job(static_cast<AddToLibraryJob *>(data));
  std::set<BString> paths;
  std::set<node_ref> visited;
  for (const entry_ref &ref : job->refs)
    CollectAudioFiles(ref, paths, visited);

  // The cache skips files it already has, and its scan reports how many
  // were new when it is done.
  BMessage scan(MSG_SCAN_FILES);
  for (const BString &path : paths) {
    scan.AddString("path", path);
    scan.AddInt32("playlist", -1); // Not from a playlist.
  }
  DEBUG_PRINT("Add to Library: %zu audio files found\n", paths.size());

  BMessage status(MSG_STATUS_UPDATE);
  BString text;
  if (paths.empty())
    text = B_TRANSLATE("No music files found to add.");
  else
    text.SetToFormat(B_TRANSLATE("Adding %ld files to the library..."),
                     (long)paths.size());
  status.AddString("text", text);
  job->window.SendMessage(&status);
  if (!paths.empty())
    job->cache.SendMessage(&scan);
  return B_OK;
}

namespace {

bool IsAudioFileName(const char *name) {
  BString lower(name);
  lower.ToLower();
  static const char *exts[] = {".mp3", ".flac", ".wav", ".ogg",
                               ".m4a", ".aac",  ".wma"};
  for (const char *ext : exts)
    if (lower.EndsWith(ext))
      return true;
  return false;
}

/** Number of audio files in @p dir whose album tag is @p album. */
size_t CountAlbumFiles(const BString &dir, const BString &album) {
  BDirectory d(dir.String());
  BEntry entry;
  size_t count = 0;
  while (d.GetNextEntry(&entry) == B_OK) {
    BPath p;
    if (entry.GetPath(&p) != B_OK || !entry.IsFile() || !IsAudioFileName(p.Leaf()))
      continue;
    TagData td;
    if (MetadataTagIO::ReadTags(p, td, false) && td.album == album)
      count++;
  }
  return count;
}

void SendStatus(const BMessenger &target, const BString &text) {
  BMessage status(MSG_STATUS_UPDATE);
  status.AddString("text", text);
  target.SendMessage(&status);
}

/** Loudness workers, at most; more mostly makes disks seek. */
const size_t kMaxLoudnessWorkers = 8;

/** Copies a library item's gain to the GainStore playback reads. */
void SyncGainStore(const MediaItem &mi) {
  ReplayGainInfo gain;
  gain.trackGain = mi.rgTrackGain;
  gain.trackPeak = mi.rgTrackPeak;
  gain.albumGain = mi.rgAlbumGain;
  gain.albumPeak = mi.rgAlbumPeak;
  GainStore::Set(mi.path, gain);
}

/** Thread entry that runs a std::function<void()>. */
int32 RunFunction(void *data) {
  (*static_cast<std::function<void()> *>(data))();
  return 0;
}

} // namespace

void LibraryController::AnalyzeLoudness(BMessage *msg) {
  std::vector<BString> paths;
  BMessage files;
  if (msg->FindMessage("files", &files) == B_OK) {
    entry_ref ref;
    for (int32 i = 0; files.FindRef("refs", i, &ref) == B_OK; i++) {
      BPath p(&ref);
      if (p.InitCheck() == B_OK)
        paths.push_back(p.Path());
    }
  }
  if (paths.empty())
    return;

  if (fLoudnessRunning.exchange(true)) {
    SendStatus(BMessenger(fWindow),
               B_TRANSLATE("Loudness analysis is already running."));
    return;
  }

  BMessenger target(fWindow);
  std::atomic<bool> *shuttingDown = &fWindow->fShuttingDown;
  // Tells the window (and through it the library cache) what was written.
  auto report = [target](const BString &path, const ReplayGainInfo &gain) {
    BMessage result(MSG_LOUDNESS_PROGRESS);
    result.AddString("path", path);
    if (gain.HasTrack()) {
      result.AddFloat("rgTrackGain", gain.trackGain);
      result.AddFloat("rgTrackPeak", gain.trackPeak);
    }
    if (gain.HasAlbum()) {
      result.AddFloat("rgAlbumGain", gain.albumGain);
      result.AddFloat("rgAlbumPeak", gain.albumPeak);
    }
    target.SendMessage(&result);
  };
  std::atomic<bool> *running = &fLoudnessRunning;

  fWindow->LaunchThread("loudness_analysis", [paths, target, shuttingDown,
                                              running, report]() {
    // Group by folder and album tag: album gain needs the whole album.
    std::map<BString, std::vector<size_t>> albumMap;
    std::vector<BString> albumNames(paths.size());
    for (size_t i = 0; i < paths.size(); i++) {
      TagData td;
      MetadataTagIO::ReadTags(BPath(paths[i].String()), td, false);
      albumNames[i] = td.album;
      BPath parent;
      BPath(paths[i].String()).GetParent(&parent);
      BString key(parent.Path());
      // Untitled files never form an album with each other.
      key << '\n' << (td.album.IsEmpty() ? paths[i] : td.album);
      albumMap[key].push_back(i);
    }
    std::vector<std::vector<size_t>> albums;
    albums.reserve(albumMap.size());
    for (auto &entry : albumMap)
      albums.push_back(std::move(entry.second));

    std::atomic<size_t> nextAlbum(0), started(0), doneCount(0),
        failedCount(0), albumsTaggedCount(0), partialAlbumsCount(0);

    // Measures one album's files, then writes album gain if the selection
    // held the whole album.
    auto measureAlbum = [&](const std::vector<size_t> &members) {
      std::vector<double> blocks;
      double peak = 0;
      std::vector<BString> measured;

      for (size_t index : members) {
        if (shuttingDown->load())
          return;
        const BString &path = paths[index];
        BString text;
        text.SetToFormat(B_TRANSLATE("Analyzing loudness: %zu of %zu, %s"),
                         started.fetch_add(1) + 1, paths.size(),
                         BPath(path.String()).Leaf());
        SendStatus(target, text);

        std::unique_ptr<LoudnessMeter> meter;
        BString error;
        status_t status =
            LoudnessAnalyzer::AnalyzeFile(path.String(), shuttingDown, meter, error);
        if (status == B_CANCELED)
          return;
        if (status != B_OK || !std::isfinite(meter->Loudness())) {
          DEBUG_PRINT("Loudness: %s: %s\n", path.String(), error.String());
          failedCount++;
          continue;
        }

        ReplayGainInfo gain;
        gain.trackGain = (float)meter->Gain();
        gain.trackPeak = (float)meter->Peak();
        if (MetadataTagIO::WriteReplayGain(BPath(path.String()), gain)) {
          report(path, gain);
          doneCount++;
          measured.push_back(path);
          blocks.insert(blocks.end(), meter->Blocks().begin(),
                        meter->Blocks().end());
          peak = std::max(peak, meter->Peak());
        } else {
          failedCount++;
        }
      }

      const BString &album = albumNames[members.front()];
      if (album.IsEmpty() || measured.size() != members.size())
        return;
      BPath parent;
      BPath(measured.front().String()).GetParent(&parent);
      if (CountAlbumFiles(parent.Path(), album) != members.size()) {
        partialAlbumsCount++;
        return;
      }

      ReplayGainInfo gain;
      gain.albumGain = (float)LoudnessMeter::GainFor(
          LoudnessMeter::GatedLoudness(blocks));
      gain.albumPeak = (float)peak;
      for (const BString &path : measured) {
        if (MetadataTagIO::WriteReplayGain(BPath(path.String()), gain))
          report(path, gain);
      }
      albumsTaggedCount++;
    };

    // Albums are measured in parallel, one worker per core. Workers run below
    // normal priority so playback and the window stay responsive on a slow
    // machine. TagLib calls are serialised by TagLibLock(); decoding is not.
    std::function<void()> work = [&]() {
      for (;;) {
        const size_t a = nextAlbum.fetch_add(1);
        if (a >= albums.size() || shuttingDown->load())
          return;
        measureAlbum(albums[a]);
      }
    };

    system_info info;
    size_t cpus = get_system_info(&info) == B_OK ? info.cpu_count : 1;
    size_t workerCount =
        std::min(std::max<size_t>(cpus, 1),
                 std::min(albums.size(), kMaxLoudnessWorkers));
    std::vector<thread_id> workers;
    for (size_t i = 0; i < workerCount; i++) {
      thread_id t = spawn_thread(RunFunction, "loudness_worker",
                                 B_LOW_PRIORITY, &work);
      if (t < 0)
        break;
      workers.push_back(t);
      resume_thread(t);
    }
    if (workers.empty())
      work();
    for (thread_id t : workers) {
      status_t exitValue;
      wait_for_thread(t, &exitValue);
    }

    const size_t done = doneCount.load(), failed = failedCount.load(),
                 albumsTagged = albumsTaggedCount.load(),
                 partialAlbums = partialAlbumsCount.load();
    BString text;

    if (shuttingDown->load()) {
      running->store(false);
      return;
    }

    if (albumsTagged == 1)
      text.SetToFormat(B_TRANSLATE("Loudness analyzed: %zu tracks, 1 album"),
                       done);
    else
      text.SetToFormat(B_TRANSLATE("Loudness analyzed: %zu tracks, %zu albums"),
                       done, albumsTagged);
    if (failed > 0) {
      BString part;
      part.SetToFormat(B_TRANSLATE(", %zu could not be read or tagged"), failed);
      text << part;
    }
    if (partialAlbums > 0) {
      BString part;
      part.SetToFormat(B_TRANSLATE(", %zu partly selected albums got track "
                                   "gain only"), partialAlbums);
      text << part;
    }
    SendStatus(target, text);
    running->store(false);
  });
}

void LibraryController::HandleLoudnessResult(BMessage *msg) {
  BString path;
  if (msg->FindString("path", &path) != B_OK || !fWindow->fMediaLibraryCache)
    return;
  // Only files the library knows: the cache adds an entry for any path it is
  // sent, and a file analyzed from elsewhere doesn't belong there.
  if (fWindow->fPathIndex.find(path) == fWindow->fPathIndex.end())
    return;

  // The cache stores the values and passes the update back to the window,
  // which refreshes the row through HandleMediaItemFound().
  BMessage update(*msg);
  update.what = MSG_MEDIA_ITEM_FOUND;
  BMessenger(fWindow->fMediaLibraryCache).SendMessage(&update);
}

void LibraryController::UpdateGainPrefetch(bool listChanged) {
  AudioPlaybackEngine *engine = fWindow->fPlaybackEngine;
  const bool active =
      fWindow->fGainPrefetch && engine != nullptr && engine->IsPlaying() &&
      !engine->IsStreaming() &&
      engine->ReplayGainModeValue() != AudioPlaybackEngine::kReplayGainOff &&
      !fLoudnessRunning.load();
  if (!active) {
    if (fGainPrefetcher != nullptr)
      fGainPrefetcher->SetWanted({});
    return;
  }

  if (fGainPrefetcher == nullptr) {
    fGainPrefetcher =
        new GainPrefetcher(BMessenger(fWindow), &fWindow->fShuttingDown);
    GainPrefetcher *prefetcher = fGainPrefetcher;
    fWindow->LaunchThread("gain_prefetch",
                          [prefetcher]() { prefetcher->Run(); });
  }

  // Library tracks without a track gain: the playing one first, which takes
  // its gain as soon as it is measured, then the next one, then the rows on
  // screen, which the listener may click.
  std::vector<BString> wanted;
  std::set<BString> seen;
  auto want = [&](const BString &path) {
    auto it = fWindow->fPathIndex.find(path);
    if (it == fWindow->fPathIndex.end() || !seen.insert(path).second)
      return;
    const MediaItem &mi = fWindow->fAllItems[it->second];
    if (mi.HasTrackGain() || mi.missing || !IsAudioFileName(path.String()))
      return;
    wanted.push_back(path);
  };

  const BString current = engine->QueuePathAt(engine->CurrentIndex());
  want(current);
  if (fWindow->fPlaybackQueueManager != nullptr)
    want(engine->QueuePathAt(fWindow->fPlaybackQueueManager->PeekNextIndex()));
  const size_t priority = wanted.size();

  // Rows on screen only once the list has stayed put for a tick, so
  // scrolling through the library doesn't queue rows it only flew past. A
  // list that was just filled (a filter, search or sidebar choice) is
  // where the listener stopped, so its rows are queued at once.
  MediaTableView *cv =
      fWindow->fLibraryManager ? fWindow->fLibraryManager->ContentView() : nullptr;
  std::vector<const MediaItem *> visible;
  if (cv != nullptr)
    cv->GetVisibleItems(visible);
  const BString top = visible.empty() ? BString() : visible.front()->path;
  const bool settled = listChanged || top == fGainVisibleTop;
  fGainVisibleTop = top;
  if (settled) {
    for (const MediaItem *mi : visible)
      want(mi->path);
  }

  fGainPrefetcher->SetPlayingPath(current);
  fGainPrefetcher->SetWriteToFiles(fWindow->fGainWriteFiles);
  fGainPrefetcher->SetWanted(wanted, priority);
}

/**
 * @brief Moves a file on disk and updates library state on success.
 */
void LibraryController::HandleFileMove(BMessage *msg) {
  BString from, to;
  if (msg->FindString("from", &from) != B_OK ||
      msg->FindString("to", &to) != B_OK)
    return;
  if (from.IsEmpty() || to.IsEmpty() || from == to)
    return;

  if (to[0] != '/') {
    fWindow->UpdateStatus(B_TRANSLATE("Move failed: path must be absolute"));
    return;
  }

  BEntry entry(from.String());
  if (entry.InitCheck() != B_OK || !entry.Exists()) {
    fWindow->UpdateStatus(B_TRANSLATE("Move failed: source file not found"));
    return;
  }

  BPath toPath(to.String());
  BPath toDir;
  if (toPath.InitCheck() != B_OK || toPath.GetParent(&toDir) != B_OK ||
      !toPath.Leaf() || toPath.Leaf()[0] == '\0') {
    fWindow->UpdateStatus(B_TRANSLATE("Move failed: invalid target path"));
    return;
  }

  BDirectory dir(toDir.Path());
  if (dir.InitCheck() != B_OK) {
    fWindow->UpdateStatus(
        B_TRANSLATE("Move failed: target folder does not exist"));
    return;
  }

  BPath originalPath;
  entry.GetPath(&originalPath);

  // Tell the cache first, so it reads this before the folder watch reports
  // the move: a track Beton moves out of a sidebar folder stays in the
  // library, where one moved out in Tracker leaves it.
  if (fWindow->fMediaLibraryCache) {
    BMessage expected(MSG_FILE_MOVE_EXPECTED);
    expected.AddString("to", toPath.Path());
    fWindow->fMediaLibraryCache->PostMessage(&expected);
  }

  status_t st = entry.MoveTo(&dir, toPath.Leaf(), false);
  if (st != B_OK) {
    BString status(B_TRANSLATE("Move failed: "));
    status << strerror(st);
    fWindow->UpdateStatus(status);
    return;
  }

  // Keep Tracker's "Restore" working: it reads the original location from
  // _trk/original_path, so set it on the way into the Trash and clear it on
  // the way out (undo), as Tracker itself does.
  const bool toTrash = _IsTrashDirectory(dir);
  BNode node(&entry);
  if (node.InitCheck() == B_OK) {
    if (toTrash) {
      BString original(originalPath.Path());
      if (node.WriteAttrString(kAttrOriginalPath, &original) != B_OK)
        DEBUG_PRINT("could not write %s on %s\n", kAttrOriginalPath,
                    toPath.Path());
    } else {
      node.RemoveAttr(kAttrOriginalPath);
    }
  }

  BString newPath = toPath.Path();

  // Update the in-memory model and the visible row.
  MediaTableView *cv =
      (fWindow->fLibraryManager) ? fWindow->fLibraryManager->ContentView()
                                 : nullptr;

  auto mapIt = fWindow->fPathIndex.find(from);
  if (toTrash && mapIt != fWindow->fPathIndex.end()) {
    // A track in the Trash leaves the library. The cache keeps its entry
    // until the next full scan, so Undo brings it back as it was.
    fWindow->fAllItems.erase(fWindow->fAllItems.begin() + mapIt->second);
    RebuildPathIndex();
    if (cv && !fWindow->fIsFolderMode && !fWindow->IsPlaylistSelected()) {
      for (int32 i = 0; i < cv->CountRows(); ++i) {
        const MediaItem *mi = cv->ItemAt(i);
        if (mi && mi->path == from) {
          BRow *r = cv->RowAt(i);
          cv->RemoveRow(r);
          delete r;
          break;
        }
      }
    }
  } else if (mapIt != fWindow->fPathIndex.end()) {
    size_t idx = mapIt->second;
    fWindow->fPathIndex.erase(mapIt);
    fWindow->fAllItems[idx].path = newPath;
    fWindow->fPathIndex[newPath] = idx;
    GainStore::Remove(from);
    SyncGainStore(fWindow->fAllItems[idx]);
    if (cv)
      cv->UpdateItem(fWindow->fAllItems[idx], &from);
  } else if (cv) {
    // Items outside the library index (e.g. playlist-only entries):
    // update the visible row directly.
    for (int32 i = 0; i < cv->CountRows(); ++i) {
      const MediaItem *mi = cv->ItemAt(i);
      if (mi && mi->path == from) {
        MediaItem updated = *mi;
        updated.path = newPath;
        cv->UpdateItem(updated, &from);
        break;
      }
    }
  }

  if (cv && cv->NowPlayingPath() == from)
    cv->SetNowPlayingPath(newPath);

  if (fWindow->fLibraryManager)
    fWindow->fLibraryManager->RenameActivePath(from, newPath);

  // Rekey the persistent cache entry.
  if (fWindow->fMediaLibraryCache) {
    BMessage moved(MSG_FILE_MOVED);
    moved.AddString("from", from);
    moved.AddString("to", newPath);
    fWindow->fMediaLibraryCache->PostMessage(&moved);
  }

  // Rewrite saved playlists that reference the old path.
  if (fWindow->fPlaylistLibrary) {
    BMessage names;
    fWindow->fPlaylistLibrary->GetPlaylistNames(names, true);
    BString plName;
    for (int32 i = 0; names.FindString("name", i, &plName) == B_OK; ++i) {
      std::vector<BString> paths =
          fWindow->fPlaylistLibrary->LoadPlaylist(plName);
      bool changed = false;
      for (auto &p : paths) {
        if (p == from) {
          p = newPath;
          changed = true;
        }
      }
      if (changed)
        fWindow->fPlaylistLibrary->SavePlaylist(plName, paths);
    }
  }

  // Record the move as an undoable action (skip undo/redo replays).
  if (!msg->HasBool("undo_replay") && fWindow->fUndoManager) {
    BMessage u(MSG_FILE_MOVE);
    u.AddString("from", newPath);
    u.AddString("to", from);
    BMessage r(MSG_FILE_MOVE);
    r.AddString("from", from);
    r.AddString("to", newPath);
    fWindow->fUndoManager->RecordAction({u}, {r});
  }

  BString status(B_TRANSLATE("Moved to "));
  status << newPath;
  fWindow->UpdateStatus(status);
}

void LibraryController::HandleFileMoved(BMessage *msg) {
  BString from, to;
  if (msg->FindString("from", &from) != B_OK ||
      msg->FindString("to", &to) != B_OK)
    return;

  DEBUG_PRINT("file moved: %s -> %s\n", from.String(), to.String());

  // Beton's own moves were already applied by HandleFileMove, so for those
  // nothing below matches |from| any more and this is a no-op.
  auto mapIt = fWindow->fPathIndex.find(from);
  MediaTableView *view = fWindow->fLibraryManager
                             ? fWindow->fLibraryManager->ContentView()
                             : nullptr;
  if (mapIt != fWindow->fPathIndex.end() &&
      MediaLibraryCache::IsInTrash(to)) {
    // Moved to the Trash outside Beton: it leaves the library view, as when
    // Beton trashes it (the cache keeps it until the Trash is emptied).
    fWindow->fAllItems.erase(fWindow->fAllItems.begin() + mapIt->second);
    RebuildPathIndex();
    if (view && !fWindow->fIsFolderMode && !fWindow->IsPlaylistSelected()) {
      for (int32 i = 0; i < view->CountRows(); ++i) {
        const MediaItem *mi = view->ItemAt(i);
        if (mi && mi->path == from) {
          BRow *r = view->RowAt(i);
          view->RemoveRow(r);
          delete r;
          break;
        }
      }
      return;
    }
  } else if (mapIt != fWindow->fPathIndex.end()) {
    size_t idx = mapIt->second;
    fWindow->fPathIndex.erase(mapIt);
    fWindow->fAllItems[idx].path = to;
    fWindow->fPathIndex[to] = idx;
    GainStore::Remove(from);
    SyncGainStore(fWindow->fAllItems[idx]);
  }

  MediaTableView *cv =
      (fWindow->fLibraryManager) ? fWindow->fLibraryManager->ContentView()
                                 : nullptr;
  if (!cv)
    return;

  for (int32 i = 0; i < cv->CountRows(); ++i) {
    const MediaItem *mi = cv->ItemAt(i);
    if (!mi || mi->path != from)
      continue;
    // Moved out of the folder on show (e.g. Tracker's Move to Trash): the row
    // goes. Anywhere else it just follows the file.
    if (fWindow->fIsFolderMode &&
        !fWindow->fLibraryManager->IsPathAllowed(to, false)) {
      BRow *r = cv->RowAt(i);
      cv->RemoveRow(r);
      delete r;
    } else {
      MediaItem updated = *mi;
      updated.path = to;
      cv->UpdateItem(updated, &from);
    }
    break;
  }

  if (cv->NowPlayingPath() == from)
    cv->SetNowPlayingPath(to);
  fWindow->fLibraryManager->RenameActivePath(from, to);
}

/**
 * @brief Replaces current item cache with persisted entries and refreshes UI.
 */
void LibraryController::HandleCacheLoaded() {
  DEBUG_PRINT("MSG_CACHE_LOADED received\n");
  bigtime_t tc0 = system_time();
  fWindow->fCacheLoaded = true;
  if (fWindow->fMediaLibraryCache) {
    auto entries = fWindow->fMediaLibraryCache->AllEntries();
    bigtime_t tc1 = system_time();
    fWindow->fAllItems = std::move(entries);
    for (const MediaItem &mi : fWindow->fAllItems)
      SyncGainStore(mi);

    RebuildPathIndex();
    bigtime_t tc2 = system_time();

    DEBUG_PRINT("Cache populated: %zu items "
                "(AllEntries=%lld us, PathIndex=%lld us)\n",
                fWindow->fAllItems.size(), (long long)(tc1 - tc0),
                (long long)(tc2 - tc1));

    fWindow->UpdateFilteredViews();
    bigtime_t tc3 = system_time();
    DEBUG_PRINT("fWindow->UpdateFilteredViews took %lld us\n",
                (long long)(tc3 - tc2));
    if (fWindow->fStatusBarController)
      fWindow->fStatusBarController->UpdateLibraryStatus();
  }
}

/**
 * @brief Rebuilds the library after a corrupt cache file was discarded.
 *
 * Recovering by hand used to mean deleting media.cache and then clicking every
 * folder and playlist in the sidebar one at a time, which is neither obvious
 * nor pleasant. This does the whole thing automatically:
 *   - configured music sources and sidebar folders come back via a full scan
 *     (MediaLibraryCache::StartScan reads both from the settings file);
 *   - playlist tracks are queued explicitly, because a playlist may reference
 *     files that live outside every scanned folder and would otherwise stay
 *     bare filename-only rows.
 */
void LibraryController::HandleCacheCorrupt(BMessage *msg) {
  int32 recovered = 0;
  int32 declared = 0;
  BString backupPath;
  if (msg) {
    msg->FindInt32("recovered", &recovered);
    msg->FindInt32("declared", &declared);
    msg->FindString("backup", &backupPath);
  }

  DEBUG_PRINT("MSG_CACHE_CORRUPT received (%ld entries salvaged), "
              "rebuilding library\n",
              (long)recovered);

  if (recovered > 0) {
    // The salvaged entries are already loaded and verified against disk, so
    // the views must keep them: the scan below fast-skips those files and
    // will not report them again.
    BString status;
    status.SetToFormat(
        B_TRANSLATE("Media cache was damaged. Recovered %ld entries, "
                    "scanning for the rest..."),
        (long)recovered);
    fWindow->UpdateStatus(status);
  } else {
    fWindow->UpdateStatus(
        B_TRANSLATE("Media cache was damaged and has been discarded. "
                    "Rebuilding library..."));

    if (fWindow->fLibraryManager) {
      fWindow->fLibraryManager->ContentView()->Clear();
      fWindow->fLibraryManager->GenreView()->Clear();
      fWindow->fLibraryManager->ArtistView()->Clear();
      fWindow->fLibraryManager->AlbumView()->Clear();
    }
    fWindow->fAllItems.clear();
    fWindow->fPathIndex.clear();
  }

  _ShowCacheDamagedAlert(recovered, declared, backupPath);
  _StartLibraryRebuild();
}

void LibraryController::HandleCacheEmpty(BMessage *msg) {
  int32 sources = 0;
  if (msg)
    msg->FindInt32("sources", &sources);

  const int32 collections = _CountSidebarCollections();

  DEBUG_PRINT("MSG_CACHE_EMPTY received (%ld sources, %ld sidebar "
              "collections)\n",
              (long)sources, (long)collections);

  // A first run with nothing set up yet: an empty library is the correct
  // answer, so do not launch a scan that has nothing to look at.
  if (sources <= 0 && collections <= 0)
    return;

  fWindow->UpdateStatus(
      B_TRANSLATE("No media cache found. Building the library..."));

  _StartLibraryRebuild();
}

void LibraryController::_StartLibraryRebuild() {
  if (!fWindow->fMediaLibraryCache)
    return;

  BMessenger cache(fWindow->fMediaLibraryCache);

  // Start the folder scan first: the queued file paths below are only drained
  // once the scanners are done, and anything they already recovered is
  // skipped rather than read twice.
  cache.SendMessage(MSG_RESCAN);

  if (!fWindow->fPlaylistLibrary)
    return;

  BMessage names;
  fWindow->fPlaylistLibrary->GetPlaylistNames(names, false);

  // Each path carries the playlist it came from so the cache can report how
  // many playlists actually hold work. Paths are deduplicated within a
  // playlist but not across them: a track in two playlists is scanned once,
  // yet both playlists count as having something to do.
  BMessage scanFiles(MSG_SCAN_FILES);
  int32 queued = 0;
  BString plName;

  for (int32 i = 0; names.FindString("name", i, &plName) == B_OK; ++i) {
    std::set<BString> seen;
    for (const auto &path : fWindow->fPlaylistLibrary->LoadPlaylist(plName)) {
      if (path.IsEmpty() || path[0] != '/')
        continue;
      if (!seen.insert(path).second)
        continue;
      scanFiles.AddString("path", path);
      scanFiles.AddInt32("playlist", i);
      queued++;
    }
  }

  if (queued == 0)
    return;

  cache.SendMessage(&scanFiles);

  DEBUG_PRINT("Queued %ld playlist tracks for recaching\n", (long)queued);
}

int32 LibraryController::_CountSidebarCollections() const {
  if (!fWindow->fPlaylistLibrary || !fWindow->fPlaylistLibrary->View())
    return 0;

  PlaylistSidebarView *sidebar = fWindow->fPlaylistLibrary->View();
  int32 count = 0;

  for (int32 i = 0; i < sidebar->CountItems(); ++i) {
    const PlaylistItemKind kind = sidebar->KindAt(i);
    if (kind == PlaylistItemKind::Folder || kind == PlaylistItemKind::Playlist)
      count++;
  }

  return count;
}

void LibraryController::_ShowCacheDamagedAlert(int32 recovered, int32 declared,
                                               const BString &backupPath) {
  BString text;

  if (recovered > 0) {
    // Only quote a total when the damaged header actually gave us a credible
    // one; on a badly mangled file it can be missing or lower than what was
    // recovered, and "1522 of 0 entries" would just be confusing.
    if (declared > recovered) {
      text.SetToFormat(
          B_TRANSLATE("Beton's media cache was damaged and could not be read "
                      "in full.\n\n"
                      "%ld of %ld entries were recovered and checked against "
                      "the files on disk. The rest of the library is being "
                      "rescanned now."),
          (long)recovered, (long)declared);
    } else {
      text.SetToFormat(
          B_TRANSLATE("Beton's media cache was damaged and could not be read "
                      "in full.\n\n"
                      "%ld entries were recovered and checked against the "
                      "files on disk. The rest of the library is being "
                      "rescanned now."),
          (long)recovered);
    }
  } else {
    text = B_TRANSLATE("Beton's media cache was damaged and could not be "
                       "read.\n\n"
                       "It has been discarded, and the library is being "
                       "rebuilt by rescanning your music folders and "
                       "playlists.");
  }

  text << "\n\n"
       << B_TRANSLATE("You can keep using Beton while this runs.");

  if (!backupPath.IsEmpty()) {
    text << "\n\n"
         << B_TRANSLATE("A copy of the damaged file has been kept, should you "
                        "want to examine it:")
         << "\n" << backupPath;
  }

  BAlert *alert =
      new BAlert(B_TRANSLATE("Media cache damaged"), text.String(),
                 B_TRANSLATE("OK"), NULL, NULL, B_WIDTH_AS_USUAL,
                 B_WARNING_ALERT);
  alert->SetShortcut(0, B_ESCAPE);

  // BAlert's width_style only sizes the buttons, so the text view has to be
  // widened by hand to keep the backup path on one line. Broken across lines
  // at an arbitrary character it reads as a different filename than the one
  // actually on disk. Capped so a deeply nested path cannot produce an alert
  // wider than the screen.
  if (!backupPath.IsEmpty()) {
    if (BTextView *view = alert->TextView()) {
      const float pathWidth = view->StringWidth(backupPath.String());
      view->SetExplicitMinSize(
          BSize(std::min(pathWidth + 16.0f, 700.0f), B_SIZE_UNSET));
    }
  }

  // Asynchronous on purpose: the rebuild is already under way and this runs
  // on the window's looper, so a modal Go() would freeze the UI behind it.
  alert->Go(NULL);
}

/**
 * @brief Starts a complete rescan and clears current visible library content.
 */
void LibraryController::StartFullRescan() {
  DEBUG_PRINT("Rescan triggered\n");

  fWindow->fLibraryManager->ContentView()->Clear();
  fWindow->fLibraryManager->GenreView()->Clear();
  fWindow->fLibraryManager->ArtistView()->Clear();
  fWindow->fLibraryManager->AlbumView()->Clear();
  fWindow->fAllItems.clear();

  if (fWindow->fMediaLibraryCache) {
    BMessenger(fWindow->fMediaLibraryCache).SendMessage(MSG_RESCAN);
  }

  fWindow->fStatusLabel->SetText(B_TRANSLATE("Rescan started..."));
}

/** @brief Formats a count with the locale's thousands separators. */
static BString FormatCount(int32 value) {
  BString out;
  static BNumberFormat format;
  if (format.Format(out, (int32)value) != B_OK)
    out.SetToFormat("%ld", (long)value);
  return out;
}

/** @brief Formats a duration as m:ss, or h:mm:ss once it runs past an hour. */
static BString FormatDuration(int64 seconds) {
  if (seconds < 0)
    seconds = 0;

  BString out;
  const int64 hours = seconds / 3600;
  const int64 minutes = (seconds % 3600) / 60;
  const int64 secs = seconds % 60;

  if (hours > 0) {
    out.SetToFormat("%lld:%02lld:%02lld", (long long)hours,
                    (long long)minutes, (long long)secs);
  } else {
    out.SetToFormat("%lld:%02lld", (long long)minutes, (long long)secs);
  }
  return out;
}

/**
 * @brief Updates status text while scan is running.
 */
void LibraryController::UpdateScanProgress(BMessage *msg) {
  if (msg->FindInt32("files_total", &fScanFilesTotal) != B_OK)
    return;

  msg->FindInt32("folders", &fScanFolders);
  msg->FindInt32("playlists", &fScanPlaylists);
  msg->FindInt32("files_done", &fScanFilesDone);

  // Anchor the clock off the cache's elapsed figure so the label keeps
  // counting between reports without drifting away from the real scan.
  int64 elapsedSec = 0;
  msg->FindInt64("elapsed_sec", &elapsedSec);
  fScanStartTime = system_time() - (bigtime_t)elapsedSec * 1000000;

  _StartScanStatusTimer();
  _RenderScanStatus();
}

void LibraryController::TickScanStatus() { _RenderScanStatus(); }

void LibraryController::_RenderScanStatus() {
  if (!fWindow->fStatusLabel)
    return;

  if (fScanFilesTotal <= 0) {
    fWindow->fStatusLabel->SetText(B_TRANSLATE("Checking for changes..."));
    return;
  }

  // Scope: fixed for the run, and omitted entirely when it would only say
  // "0 Folders" — a playlist-only pass has no folders and vice versa.
  BString scope;
  if (fScanFolders > 0) {
    scope.SetToFormat(B_TRANSLATE("%ld Folders"), (long)fScanFolders);
  }
  if (fScanPlaylists > 0) {
    if (!scope.IsEmpty())
      scope << ", ";
    BString part;
    part.SetToFormat(B_TRANSLATE("%ld Playlists"), (long)fScanPlaylists);
    scope << part;
  }

  const int64 elapsedSec = (system_time() - fScanStartTime) / 1000000;
  const int32 done = std::min(fScanFilesDone, fScanFilesTotal);

  // Alternate the trailing label every five seconds.
  const bool showRemaining = ((elapsedSec / 5) % 2) == 1;

  BString tail;
  if (showRemaining && done > 0) {
    // Linear extrapolation. Every counted file is real work — files already
    // cached were excluded from both figures — so the rate stays meaningful.
    const int64 remainingSec =
        (int64)((double)elapsedSec * (fScanFilesTotal - done) / done);
    const int32 pct = (int32)(100.0 * (fScanFilesTotal - done) / fScanFilesTotal);
    BString duration = FormatDuration(remainingSec);
    tail.SetToFormat(B_TRANSLATE("Remaining ~%s (%ld%%)"), duration.String(),
                     (long)pct);
  } else {
    const int32 pct = (int32)(100.0 * done / fScanFilesTotal);
    BString duration = FormatDuration(elapsedSec);
    tail.SetToFormat(B_TRANSLATE("Elapsed %s (%ld%%)"), duration.String(),
                     (long)pct);
  }

  BString doneStr = FormatCount(done);
  BString totalStr = FormatCount(fScanFilesTotal);

  BString status;
  if (scope.IsEmpty()) {
    status.SetToFormat(B_TRANSLATE("Scanning %s/%s Files - %s"),
                       doneStr.String(), totalStr.String(), tail.String());
  } else {
    status.SetToFormat(B_TRANSLATE("Scanning %s: %s/%s Files - %s"),
                       scope.String(), doneStr.String(), totalStr.String(),
                       tail.String());
  }

  fWindow->fStatusLabel->SetText(status.String());
}

void LibraryController::_StartScanStatusTimer() {
  if (fScanStatusRunner != nullptr)
    return;

  BMessage tick(MSG_SCAN_TICK);
  fScanStatusRunner =
      new BMessageRunner(BMessenger(fWindow), &tick, 1000000, -1);
  if (fScanStatusRunner->InitCheck() != B_OK) {
    delete fScanStatusRunner;
    fScanStatusRunner = nullptr;
  }
}

void LibraryController::_StopScanStatusTimer() {
  delete fScanStatusRunner;
  fScanStatusRunner = nullptr;
}

/**
 * @brief Finalizes scan results and reloads items from cache.
 */
void LibraryController::HandleScanDone(BMessage *msg) {
  DEBUG_PRINT("MSG_SCAN_DONE received\n");

  // Individual scanners report as they finish, but only the cache's final
  // message means the whole cycle is over. Retiring the live status on the
  // first one would freeze it while the rest were still working.
  if (msg->GetBool("final", false)) {
    _StopScanStatusTimer();

    const int64 elapsedSec =
        fScanStartTime > 0 ? (system_time() - fScanStartTime) / 1000000 : 0;
    BString duration = FormatDuration(elapsedSec);

    BString status;
    status.SetToFormat(B_TRANSLATE("Scan completed in %s, %ld new files"),
                       duration.String(), (long)fWindow->fNewFilesCount);
    fWindow->UpdateStatus(status.String(), false);

    fScanFilesDone = 0;
    fScanFilesTotal = 0;
    fScanStartTime = 0;
  }

  if (fWindow->fMediaLibraryCache) {
    auto entries = fWindow->fMediaLibraryCache->AllEntries();
    fWindow->fAllItems = std::move(entries);
    for (const MediaItem &mi : fWindow->fAllItems)
      SyncGainStore(mi);

    RebuildPathIndex();
  }

  fWindow->UpdateFilteredViews();

  fWindow->fNewFilesCount = 0;
}

/**
 * @brief Loads pending cache items in small timed batches to keep UI responsive.
 */
void LibraryController::HandleBatchTimer() {
  if (fWindow->fCurrentIndex >= (int32)fWindow->fPendingItems.size()) {
    if (fWindow->fBatchRunner) {
      delete fWindow->fBatchRunner;
      fWindow->fBatchRunner = nullptr;
    }
    DEBUG_PRINT("Cache load finished (%zu items)\n",
                fWindow->fPendingItems.size());
    if (fWindow->fStatusBarController)
      fWindow->fStatusBarController->UpdateLibraryStatus();
    return;
  }

  const int BATCH_SIZE = 200;
  int count = 0;

  while (fWindow->fCurrentIndex < (int32)fWindow->fPendingItems.size() && count < BATCH_SIZE) {
    fWindow->fLibraryManager->ContentView()->AddEntry(fWindow->fPendingItems[fWindow->fCurrentIndex]);
    fWindow->fCurrentIndex++;
    count++;
  }

  char buf[128];
  snprintf(buf, sizeof(buf), B_TRANSLATE("Loading cache... %ld/%zu"),
           (long)fWindow->fCurrentIndex, fWindow->fPendingItems.size());
  fWindow->fStatusLabel->SetText(buf);
}

/**
 * @brief Applies a batch of media field updates and schedules partial refresh.
 */
void LibraryController::HandleMediaBatch(BMessage *msg) {
  type_code type;
  int32 count = 0;
  if (msg->GetInfo("path", &type, &count) != B_OK)
    return;

  for (int32 i = 0; i < count; i++) {
    BString path;
    if (msg->FindString("path", i, &path) != B_OK)
      continue;

    bool isNewItem = false;
    MediaItem *itemToUpdate = nullptr;
    auto mapIt = fWindow->fPathIndex.find(path);
    if (mapIt != fWindow->fPathIndex.end()) {
      itemToUpdate = &fWindow->fAllItems[mapIt->second];
    } else {
      MediaItem newItem;
      newItem.path = path;
      fWindow->fAllItems.push_back(newItem);
      fWindow->fPathIndex[path] = fWindow->fAllItems.size() - 1;
      itemToUpdate = &fWindow->fAllItems.back();
      isNewItem = true;
    }

    if (itemToUpdate) {
      BString tmp;
      if (msg->FindString("title", i, &tmp) == B_OK)
        itemToUpdate->title = tmp;
      if (msg->FindString("artist", i, &tmp) == B_OK)
        itemToUpdate->artist = tmp;
      if (msg->FindString("album", i, &tmp) == B_OK)
        itemToUpdate->album = tmp;
      if (msg->FindString("genre", i, &tmp) == B_OK)
        itemToUpdate->genre = tmp;
      if (msg->FindString("albumArtist", i, &tmp) == B_OK)
        itemToUpdate->albumArtist = tmp;
      if (msg->FindString("mbAlbumId", i, &tmp) == B_OK)
        itemToUpdate->mbAlbumId = tmp;
      if (msg->FindString("mbArtistId", i, &tmp) == B_OK)
        itemToUpdate->mbArtistId = tmp;
      if (msg->FindString("mbTrackId", i, &tmp) == B_OK)
        itemToUpdate->mbTrackId = tmp;

      int32 val;
      if (msg->FindInt32("year", i, &val) == B_OK)
        itemToUpdate->year = val;
      if (msg->FindInt32("track", i, &val) == B_OK)
        itemToUpdate->track = val;
      if (msg->FindInt32("disc", i, &val) == B_OK)
        itemToUpdate->disc = val;
      if (msg->FindInt32("duration", i, &val) == B_OK)
        itemToUpdate->duration = val;
      itemToUpdate->ReadGainFields(*msg, i);
      SyncGainStore(*itemToUpdate);

      MediaTableView *cv =
          fWindow->fLibraryManager ? fWindow->fLibraryManager->ContentView() : nullptr;
      if (cv) {
        if (isNewItem) {
          if (fWindow->fIsLibraryMode ||
              (fWindow->fIsFolderMode && fWindow->fLibraryManager->IsPathAllowed(path, false))) {
            cv->AddEntry(*itemToUpdate);
          }
        } else {
          cv->UpdateItem(*itemToUpdate);
        }
      }
    }
  }

  delete fWindow->fViewsRefreshRunner;
  BMessage refresh(MSG_VIEWS_REFRESH_PARTIAL);
  fWindow->fViewsRefreshRunner =
      new BMessageRunner(BMessenger(fWindow), &refresh, 200000, 1);
}

/**
 * @brief Refreshes filtered views after debounced incremental updates.
 */
void LibraryController::RefreshPartialViews() {
  DEBUG_PRINT("Debounced partial view refresh triggered\n");
  const auto &items =
      (fWindow->fIsRadioMode || fWindow->fIsDlnaMode)
          ? fWindow->fRadioItems
          : fWindow->fAllItems;
  fWindow->fLibraryManager->UpdateFilteredViews(
      items, fWindow->fIsLibraryMode || fWindow->fIsRadioMode ||
                 fWindow->fIsDlnaMode,
      fWindow->fCurrentPlaylistName,
      fWindow->fSearchField->Text() ? fWindow->fSearchField->Text() : "",
      false, false, fWindow->IsPlaylistSelected());
}

/**
 * @brief Applies single-item updates from scanner/metadata changes.
 */
void LibraryController::HandleMediaItemFound(BMessage *msg) {
  BString path;
  if (msg->FindString("path", &path) != B_OK) {
    DEBUG_PRINT(
        "MSG_MEDIA_ITEM_FOUND path not found in message!\n");
    return;
  }

  DEBUG_PRINT("Item update path: '%s'\n", path.String());

  MediaItem *itemToUpdate = nullptr;
  auto mapIt = fWindow->fPathIndex.find(path);
  bool isLibraryItem = (mapIt != fWindow->fPathIndex.end());
  if (isLibraryItem) {
    itemToUpdate = &fWindow->fAllItems[mapIt->second];
  } else {
    MediaItem newItem;
    newItem.path = path;
    fWindow->fAllItems.push_back(newItem);
    fWindow->fPathIndex[path] = fWindow->fAllItems.size() - 1;
    itemToUpdate = &fWindow->fAllItems.back();
  }

  if (!itemToUpdate) {
    DEBUG_PRINT("MSG_MEDIA_ITEM_FOUND itemToUpdate is NULL "
                "for path: %s\n",
                path.String());
    return;
  }

  bool needsFullRefresh = false;

  BString tmp;
  if (msg->FindString("title", &tmp) == B_OK)
    itemToUpdate->title = tmp;
  if (msg->FindString("artist", &tmp) == B_OK) {
    if (itemToUpdate->artist != tmp)
      needsFullRefresh = true;
    itemToUpdate->artist = tmp;
  }
  if (msg->FindString("album", &tmp) == B_OK) {
    if (itemToUpdate->album != tmp)
      needsFullRefresh = true;
    DEBUG_PRINT("Updating Album to: %s\n", tmp.String());
    itemToUpdate->album = tmp;
  }
  if (msg->FindString("genre", &tmp) == B_OK) {
    if (itemToUpdate->genre != tmp)
      needsFullRefresh = true;
    itemToUpdate->genre = tmp;
  }
  if (msg->FindString("comment", &tmp) == B_OK)
    itemToUpdate->comment = tmp;
  if (msg->FindString("albumArtist", &tmp) == B_OK)
    itemToUpdate->albumArtist = tmp;
  if (msg->FindString("composer", &tmp) == B_OK)
    itemToUpdate->composer = tmp;
  if (msg->FindString("mbAlbumId", &tmp) == B_OK ||
      msg->FindString("mbAlbumID", &tmp) == B_OK)
    itemToUpdate->mbAlbumId = tmp;
  if (msg->FindString("mbArtistId", &tmp) == B_OK ||
      msg->FindString("mbArtistID", &tmp) == B_OK)
    itemToUpdate->mbArtistId = tmp;
  if (msg->FindString("mbTrackId", &tmp) == B_OK ||
      msg->FindString("mbTrackID", &tmp) == B_OK)
    itemToUpdate->mbTrackId = tmp;

  int32 val;
  if (msg->FindInt32("year", &val) == B_OK)
    itemToUpdate->year = val;
  if (msg->FindInt32("track", &val) == B_OK)
    itemToUpdate->track = val;
  if (msg->FindInt32("trackTotal", &val) == B_OK)
    itemToUpdate->trackTotal = val;
  if (msg->FindInt32("disc", &val) == B_OK)
    itemToUpdate->disc = val;
  if (msg->FindInt32("discTotal", &val) == B_OK)
    itemToUpdate->discTotal = val;
  if (msg->FindInt32("duration", &val) == B_OK)
    itemToUpdate->duration = val;
  if (msg->FindInt32("rating", &val) == B_OK) {
    itemToUpdate->rating = val;
    DEBUG_PRINT(
        "MSG_MEDIA_ITEM_FOUND extracted rating %ld for %s\n",
        (long)val, itemToUpdate->title.String());
  }
  if (msg->FindInt32("bitrate", &val) == B_OK)
    itemToUpdate->bitrate = val;
  itemToUpdate->ReadGainFields(*msg);
  SyncGainStore(*itemToUpdate);
  // A gain just measured for the playing track applies to it right away,
  // rather than from the next time it plays.
  if (msg->HasFloat("rgTrackGain") && fWindow->fPlaybackEngine != nullptr)
    fWindow->fPlaybackEngine->RefreshReplayGain(path);

  if (fWindow->fLibraryManager) {
    fWindow->fLibraryManager->UpdateActiveItem(*itemToUpdate);
    if (fWindow->fLibraryManager->ContentView()) {
      if (isLibraryItem) {
        fWindow->fLibraryManager->ContentView()->UpdateItem(*itemToUpdate);
      } else {
        // In Folder mode an Undo may already have put the row back (from saved
        // data) before the cache's rescan of the returned file reports it.
        // Not checked in Library mode: a scan adds rows one by one here, and a
        // per-row search of a large library would make that quadratic.
        MediaTableView *cv = fWindow->fLibraryManager->ContentView();
        if (fWindow->fIsLibraryMode ||
            (fWindow->fIsFolderMode &&
             fWindow->fLibraryManager->IsPathAllowed(path, false) &&
             !cv->HasPath(path))) {
          cv->AddEntry(*itemToUpdate);
        }
      }
    }
  }

  if (fWindow->fMetadataPropertiesWindow)
    fWindow->fMetadataPropertiesWindow->PostMessage(msg);

  if (needsFullRefresh) {
    DEBUG_PRINT("Scheduling debounced view refresh for %s...\n",
                itemToUpdate->title.String());

    // Keep Folder rows pinned. A full refresh rebuilds the content list and
    // re-applies the sort column, so editing the very field the view is sorted
    // by (Fast Edit on Artist while sorted by Artist) yanks the row to a new
    // position under the cursor. The row was already updated in place above, so
    // only the genre/artist/album filter lists still need rebuilding — and the
    // partial refresh does exactly that, leaving the content list (and with it
    // the row order and scroll position) untouched. Playlist mode has its own
    // lock via the position column; Library mode keeps re-sorting as before.
    BMessage refresh(fWindow->fIsFolderMode ? MSG_VIEWS_REFRESH_PARTIAL
                                            : MSG_VIEWS_REFRESH);
    delete fWindow->fViewsRefreshRunner;
    fWindow->fViewsRefreshRunner =
        new BMessageRunner(BMessenger(fWindow), &refresh, 200000, 1);
  }
}

/**
 * @brief Removes a media item from current view and backing store.
 */
void LibraryController::HandleMediaItemRemoved(BMessage *msg) {
  BString path;
  if (msg->FindString("path", &path) != B_OK)
    return;

  DEBUG_PRINT("remove item: %s\n", path.String());

  MediaTableView *cv = fWindow->fLibraryManager->ContentView();
  for (int32 i = 0; i < cv->CountRows(); ++i) {
    const MediaItem *mi = cv->ItemAt(i);
    if (mi && mi->path == path) {
      BRow *r = cv->RowAt(i);
      cv->RemoveRow(r);
      delete r;
      break;
    }
  }

  auto mapIt = fWindow->fPathIndex.find(path);
  if (mapIt != fWindow->fPathIndex.end()) {
    fWindow->fAllItems.erase(fWindow->fAllItems.begin() + mapIt->second);
    RebuildPathIndex();
  }
}

/**
 * @brief Rebuilds lookup table from media path to vector index.
 */
void LibraryController::RebuildPathIndex() {
  fWindow->fPathIndex.clear();
  for (size_t i = 0; i < fWindow->fAllItems.size(); ++i) {
    fWindow->fPathIndex[fWindow->fAllItems[i].path] = i;
  }
}

struct TrackerRevealEntry {
  entry_ref fileRef;
  BString   dirPath;
};

// Runs in a detached thread: waits for Tracker windows to settle, then
// walks the Tracker scripting hierarchy to find each parent directory
// window and sets its Selection property to the corresponding file.
static int32 _TrackerSelectThread(void *data) {
  auto *entries = static_cast<std::vector<TrackerRevealEntry> *>(data);

  BMessenger tracker("application/x-vnd.Be-TRAK");
  if (!tracker.IsValid()) {
    delete entries;
    return B_OK;
  }

  // Gather refs per window index to batch into one B_SET_PROPERTY each.
  // Retry while the freshly launched window has not appeared yet.
  std::map<int32, std::vector<entry_ref>> windowSelections;

  for (int32 attempt = 0; attempt < 8; ++attempt) {
    snooze(300000); // 300 ms between polls for the new window
    windowSelections.clear();

    BMessage countMsg(B_COUNT_PROPERTIES);
    countMsg.AddSpecifier("Window");
    BMessage countReply;
    if (tracker.SendMessage(&countMsg, &countReply, 0, 2000000) != B_OK)
      continue;
    int32 windowCount = 0;
    countReply.FindInt32("result", &windowCount);

    size_t matched = 0;
    for (auto &e : *entries) {
      for (int32 i = 0; i < windowCount; ++i) {
        BMessage pathMsg(B_GET_PROPERTY);
        pathMsg.AddSpecifier("Path");
        pathMsg.AddSpecifier("Poses");
        pathMsg.AddSpecifier("Window", i);

        BMessage pathReply;
        if (tracker.SendMessage(&pathMsg, &pathReply, 0, 500000) != B_OK)
          continue;

        // Tracker replies with the window's directory as an entry_ref.
        entry_ref dirRef;
        if (pathReply.FindRef("result", &dirRef) != B_OK)
          continue;
        BPath wPath(&dirRef);
        if (wPath.InitCheck() != B_OK)
          continue;

        if (e.dirPath == wPath.Path()) {
          windowSelections[i].push_back(e.fileRef);
          ++matched;
          break;
        }
      }
    }

    if (matched == entries->size())
      break;
  }

  for (auto &[winIdx, fileRefs] : windowSelections) {
    BMessage sel(B_SET_PROPERTY);
    sel.AddSpecifier("Selection");
    sel.AddSpecifier("Poses");
    sel.AddSpecifier("Window", winIdx);
    for (auto &ref : fileRefs)
      sel.AddRef("data", &ref);
    tracker.SendMessage(&sel);
  }

  delete entries;
  return B_OK;
}

/**
 * @brief Opens parent directories of provided refs in Tracker and selects
 *        the files within those windows.
 */
void LibraryController::RevealInTracker(BMessage *msg) {
  std::vector<entry_ref> refs;
  BMessage files;
  if (msg->FindMessage("files", &files) == B_OK) {
    entry_ref r;
    for (int32 i = 0; files.FindRef("refs", i, &r) == B_OK; ++i)
      refs.push_back(r);
  } else {
    entry_ref r;
    for (int32 i = 0; msg->FindRef("refs", i, &r) == B_OK; ++i)
      refs.push_back(r);
  }
  if (refs.empty())
    return;

  auto *threadEntries = new std::vector<TrackerRevealEntry>();
  std::set<BString> openedDirs;

  for (const auto &fileRef : refs) {
    BEntry e(&fileRef, true);
    BPath filePath;
    if (e.GetPath(&filePath) != B_OK)
      continue;

    BPath dirPath(filePath);
    if (dirPath.GetParent(&dirPath) != B_OK)
      continue;

    BString dirStr = dirPath.Path();
    if (openedDirs.insert(dirStr).second) {
      entry_ref dirRef;
      if (get_ref_for_path(dirStr.String(), &dirRef) == B_OK) {
        BRoster roster;
        status_t st = roster.Launch(&dirRef);
        if (st != B_OK && st != B_ALREADY_RUNNING) {
          DEBUG_PRINT("Tracker Launch dir failed: %s\n", strerror(st));
        }
      }
    }

    TrackerRevealEntry entry;
    entry.fileRef = fileRef;
    entry.dirPath = dirStr;
    threadEntries->push_back(entry);
  }

  thread_id tid = spawn_thread(_TrackerSelectThread, "tracker_reveal",
                               B_NORMAL_PRIORITY, threadEntries);
  if (tid >= 0)
    resume_thread(tid);
  else
    delete threadEntries;
}
