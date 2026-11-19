/*
 * Copyright 2005-2026, Daniel Weber.
 * Copyright 2026, David Beckman, indispensablelabs@gmail.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "ArtworkController.h"

#include "Config.h"
#include "MediaTableView.h"
#include "NowPlayingInfoPanel.h"
#include "LibraryBrowserController.h"
#include "MainWindow.h"
#include "MediaItem.h"
#include "Messages.h"
#include "MetadataService.h"
#include "PlaybackTransportController.h"
#include "RadioStationController.h"
#include "MetadataTagIO.h"

#include <Bitmap.h>
#include <Catalog.h>
#include <ColumnListView.h>
#include <DataIO.h>
#include <Entry.h>
#include <File.h>
#include <FilePanel.h>
#include <HttpRequest.h>
#include <MenuItem.h>
#include <Message.h>
#include <Messenger.h>
#include <OS.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <TranslationUtils.h>
#include <Url.h>
#include <UrlProtocolRoster.h>
#include <fs_info.h>
#include <memory>

#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "ArtworkController"

static bool IsCddaPath(const BString &path) {
  if (path.IsEmpty())
    return false;
  entry_ref ref;
  if (get_ref_for_path(path.String(), &ref) == B_OK) {
    fs_info info;
    if (fs_stat_dev(ref.device, &info) == B_OK) {
      return (strcmp(info.fsh_name, "cdda") == 0);
    }
  }
  return false;
}

/**
 * @brief Constructs the artwork controller.
 * @param window Owning main window context.
 */
ArtworkController::ArtworkController(MainWindow *window) : fWindow(window) {}

/**
 * @brief Toggles artwork visibility and updates persisted settings.
 */
void ArtworkController::ToggleArtworkVisible() {
  if (!fWindow)
    return;

  fWindow->fShowCoverArt = !fWindow->fShowCoverArt;
  if (fWindow->fViewCoverItem)
    fWindow->fViewCoverItem->SetMarked(fWindow->fShowCoverArt);
  if (fWindow->fNowPlayingInfoPanel)
    fWindow->fNowPlayingInfoPanel->SetCoverVisible(fWindow->fShowCoverArt);
  fWindow->SaveSettings();
}

/**
 * @brief Enables artwork visibility and updates persisted settings.
 */
void ArtworkController::ShowCoverArt() {
  if (!fWindow)
    return;

  fWindow->fShowCoverArt = true;
  if (fWindow->fViewCoverItem)
    fWindow->fViewCoverItem->SetMarked(true);
  if (fWindow->fNowPlayingInfoPanel)
    fWindow->fNowPlayingInfoPanel->SetCoverVisible(true);
  fWindow->SaveSettings();
}

/**
 * @brief Handles asynchronous cover-bitmap responses.
 *
 * Applies the bitmap only when it matches current UI/playback context.
 */
void ArtworkController::HandleCoverBitmapReady(BMessage *msg) {
  if (!fWindow || !msg)
    return;

  BBitmap *bmp = nullptr;
  msg->FindPointer("bitmap", (void **)&bmp);

  BString path;
  if (msg->FindString("path", &path) != B_OK) {
    delete bmp;
    return;
  }

  BString cddaCacheKey;
  if (msg->FindString("cdda_cache_key", &cddaCacheKey) == B_OK && !cddaCacheKey.IsEmpty()) {
    if (fCddaCoverCache.find(cddaCacheKey) == fCddaCoverCache.end()) {
      if (bmp && bmp->IsValid()) {
        fCddaCoverCache[cddaCacheKey] = new BBitmap(bmp);
      } else {
        fCddaCoverCache[cddaCacheKey] = nullptr;
      }
    }
  }

  if (fWindow->fRadioStationController &&
      fWindow->fRadioStationController->IsCurrentCoverDownloadThread(
          find_thread(nullptr))) {
    fWindow->fRadioStationController->MarkCoverDownloadThreadDone();
  }

  MediaTableView *cv = fWindow->fLibraryManager->ContentView();
  BRow *row = cv->CurrentSelection();
  bool match = false;
  if (row) {
    int32 idx = cv->IndexOf(row);
    const MediaItem *mi = cv->ItemAt(idx);
    if (mi && path == mi->path)
      match = true;
  }

  if (!match && path == cv->NowPlayingPath())
    match = true;

  if (!match && fWindow->fIsRadioMode && fWindow->fRadioStationController &&
      path == fWindow->fRadioStationController->ActiveStationUrl())
    match = true;

  if (!match && fWindow->fIsRadioMode && path.StartsWith("http") &&
      fWindow->fRadioStationController &&
      path == fWindow->fRadioStationController->ActiveStreamCoverUrl()) {
    match = true;
  }

  if (!match) {
    std::vector<BString> targets = GetMainCoverTargetPaths();
    for (const auto &t : targets) {
      if (t == path) {
        match = true;
        break;
      }
    }
  }

  if (match && fWindow->fNowPlayingInfoPanel && fWindow->fShowCoverArt && bmp) {
    fWindow->fNowPlayingInfoPanel->SetCover(bmp);
    if (fWindow->fIsRadioMode && path.StartsWith("http")) {
      if (fWindow->fRadioStationController)
        fWindow->fRadioStationController->StoreActiveCover(bmp);
    }
  }

  delete bmp;
}

/**
 * @brief Starts cover lookup for the current now-playing item.
 *
 * Tries explicit cover URL first, then falls back to embedded artwork.
 */
void ArtworkController::FetchNowPlayingCover(const BString &path,
                                             bool isStream) {
  if (!fWindow->fShowCoverArt || !fWindow->fNowPlayingInfoPanel || path.IsEmpty())
    return;

  fWindow->fNowPlayingInfoPanel->ClearCover();

  BString urlToDownload;
  const MediaItem *nowPlaying =
      fWindow->fPlaybackTransportController
          ? fWindow->fPlaybackTransportController->NowPlayingItem()
          : nullptr;
  if (nowPlaying && !nowPlaying->coverUrl.IsEmpty()) {
    urlToDownload = nowPlaying->coverUrl;
  } else if (isStream) {
    for (const auto &mi : fWindow->fRadioItems) {
      if (mi.path == path && !mi.coverUrl.IsEmpty()) {
        urlToDownload = mi.coverUrl;
        break;
      }
    }
  }

  if (!urlToDownload.IsEmpty())
    DownloadCoverBitmap(path, urlToDownload);
  else
    FetchEmbeddedCoverBitmap(path);
}

/**
 * @brief Downloads artwork from a remote URL in a background thread.
 */
void ArtworkController::DownloadCoverBitmap(const BString &path,
                                            const BString &coverUrl) {
  BMessenger target(fWindow);
  fWindow->LaunchThread("dlna_cover_dl", [coverUrl, target, path]() {
    BMallocIO sink;
#if B_HAIKU_VERSION <= B_HAIKU_VERSION_1_BETA_5
    BUrl burl(coverUrl.String());
#else
    BUrl burl(coverUrl.String(), true);
#endif
    std::unique_ptr<BPrivate::Network::BUrlRequest> req(
        BPrivate::Network::BUrlProtocolRoster::MakeRequest(burl, &sink));
    if (req) {
      if (auto http =
              dynamic_cast<BPrivate::Network::BHttpRequest *>(req.get())) {
        http->SetFollowLocation(true);
      }
      thread_id tid = req->Run();
      status_t exit;
      if (wait_for_thread_etc(tid, B_RELATIVE_TIMEOUT, 5000000, &exit) ==
          B_OK) {
        sink.Seek(0, SEEK_SET);
        BBitmap *bitmap = BTranslationUtils::GetBitmap(&sink);
        if (bitmap) {
          BMessage update(MSG_COVER_BITMAP_READY);
          update.AddString("path", path);
          update.AddPointer("bitmap", bitmap);
          if (target.SendMessage(&update) != B_OK)
            delete bitmap;
        }
      } else {
        req->Stop();
      }
    }
  });
}

/**
 * @brief Extracts embedded artwork from a local media file asynchronously.
 */
void ArtworkController::FetchEmbeddedCoverBitmap(const BString &path) {
  if (IsCddaPath(path)) {
    FetchCddaCoverBitmap(path);
    return;
  }

  BMessenger target(fWindow);
  BString pathStr = path;
  fWindow->LaunchThread("CoverFetch", [target, pathStr]() {
    BPath p(pathStr.String());
    CoverBlob cb;
    BBitmap *bmp = nullptr;

    if (MetadataTagIO::ExtractEmbeddedCover(p, cb) && cb.data() && cb.size() > 0) {
      BMemoryIO io(cb.data(), cb.size());
      bmp = BTranslationUtils::GetBitmap(&io);
    }

    if (target.IsValid()) {
      BMessage reply(MSG_COVER_BITMAP_READY);
      reply.AddString("path", pathStr);
      if (bmp)
        reply.AddPointer("bitmap", bmp);
      if (target.SendMessage(&reply) != B_OK)
        delete bmp;
    } else {
      delete bmp;
    }
  });
}

void ArtworkController::FetchCddaCoverBitmap(const BString &path) {
  if (!fWindow || !fWindow->fShowCoverArt || path.IsEmpty())
    return;

  BNode node(path.String());
  if (node.InitCheck() != B_OK)
    return;

  char buf[512];
  BString artist;
  BString album;

  memset(buf, 0, sizeof(buf));
  if (node.ReadAttr("Audio:Artist", B_STRING_TYPE, 0, buf, sizeof(buf) - 1) > 0)
    artist = buf;
  else if (node.ReadAttr("Media:Artist", B_STRING_TYPE, 0, buf, sizeof(buf) - 1) > 0)
    artist = buf;
  else if (node.ReadAttr("cdda/artist", B_STRING_TYPE, 0, buf, sizeof(buf) - 1) > 0)
    artist = buf;

  memset(buf, 0, sizeof(buf));
  if (node.ReadAttr("Audio:Album", B_STRING_TYPE, 0, buf, sizeof(buf) - 1) > 0)
    album = buf;
  else if (node.ReadAttr("Media:Title", B_STRING_TYPE, 0, buf, sizeof(buf) - 1) > 0)
    album = buf;
  else if (node.ReadAttr("cdda/album", B_STRING_TYPE, 0, buf, sizeof(buf) - 1) > 0)
    album = buf;

  artist.Trim();
  album.Trim();

  BString cacheKey;
  if (!artist.IsEmpty() || !album.IsEmpty()) {
    cacheKey << artist << "\t" << album;
  } else {
    BPath p(path.String());
    if (p.GetParent(&p) == B_OK) {
      cacheKey = p.Path();
    } else {
      cacheKey = path;
    }
  }

  auto it = fCddaCoverCache.find(cacheKey);
  if (it != fCddaCoverCache.end()) {
    if (it->second != nullptr && it->second->IsValid()) {
      BBitmap *cachedBmp = new BBitmap(it->second);
      BMessage reply(MSG_COVER_BITMAP_READY);
      reply.AddString("path", path);
      reply.AddPointer("bitmap", cachedBmp);
      fWindow->PostMessage(&reply);
    }
    return;
  }

  BMessenger target(fWindow);
  BString pathStr = path;
  MainWindow *win = fWindow;

  fWindow->LaunchThread("CDDACoverFetch", [target, pathStr, artist, album, cacheKey, win]() {
    BBitmap *bmp = nullptr;

    if (win->fMbClient && (!artist.IsEmpty() || !album.IsEmpty())) {
      std::vector<MBHit> hits = win->fMbClient->SearchRelease(artist, album);
      if (hits.empty() && !artist.IsEmpty() && !album.IsEmpty()) {
        hits = win->fMbClient->SearchRecording(artist, album, "");
      }

      if (!hits.empty()) {
        BString releaseId = hits[0].releaseId;
        if (!releaseId.IsEmpty()) {
          std::vector<uint8_t> imageData;
          BString mime;
          if (win->fMbClient->FetchCover(releaseId, imageData, &mime, 500, false)) {
            if (!imageData.empty()) {
              BMemoryIO io(imageData.data(), imageData.size());
              bmp = BTranslationUtils::GetBitmap(&io);
            }
          }
        }
      }
    }

    if (target.IsValid()) {
      BMessage reply(MSG_COVER_BITMAP_READY);
      reply.AddString("path", pathStr);
      reply.AddString("cdda_cache_key", cacheKey);
      if (bmp)
        reply.AddPointer("bitmap", bmp);
      if (target.SendMessage(&reply) != B_OK)
        delete bmp;
    } else {
      delete bmp;
    }
  });
}

/**
 * @brief Applies dropped cover bytes to a single file.
 */
void ArtworkController::ApplyAlbumCover(BMessage *msg) {
  const void *data = nullptr;
  ssize_t size = 0;
  BString filePath;
  if (msg->FindString("file", &filePath) == B_OK &&
      msg->FindData("bytes", B_RAW_TYPE, &data, &size) == B_OK && size > 0) {
    fWindow->fMetadataService->ApplyAlbumCover(filePath, data, size);

    BMemoryIO io(data, (size_t)size);
    BBitmap *bmp = BTranslationUtils::GetBitmap(&io);
    if (bmp && bmp->IsValid()) {
      if (fWindow->fNowPlayingInfoPanel && fWindow->fShowCoverArt)
        fWindow->fNowPlayingInfoPanel->SetCover(bmp);
      delete bmp;
    }

    FetchEmbeddedCoverBitmap(filePath);
    fWindow->UpdateFileInfo();
  }
}

/**
 * @brief Clears embedded artwork from a single file.
 */
void ArtworkController::ClearAlbumCover(BMessage *msg) {
  BString filePath;
  if (msg->FindString("file", &filePath) == B_OK) {
    fWindow->fMetadataService->ClearAlbumCover(filePath);
    if (fWindow->fNowPlayingInfoPanel)
      fWindow->fNowPlayingInfoPanel->ClearCover();
    fWindow->UpdateFileInfo();
  }
}

/**
 * @brief Applies cover updates to multiple files and refreshes visible item.
 */
void ArtworkController::ApplyDroppedCoverToAll(BMessage *msg) {
  if (!fWindow || !msg)
    return;

  fWindow->fMetadataService->ApplyCoverToAll(msg);

  bool clearCover = false;
  msg->FindBool("clear_cover", &clearCover);

  if (clearCover) {
    if (fWindow->fNowPlayingInfoPanel)
      fWindow->fNowPlayingInfoPanel->ClearCover();
  } else {
    const void *data = nullptr;
    ssize_t size = 0;
    if (msg->FindData("bytes", B_RAW_TYPE, &data, &size) == B_OK && data && size > 0) {
      BMemoryIO io(data, (size_t)size);
      BBitmap *bmp = BTranslationUtils::GetBitmap(&io);
      if (bmp && bmp->IsValid()) {
        if (fWindow->fNowPlayingInfoPanel && fWindow->fShowCoverArt)
          fWindow->fNowPlayingInfoPanel->SetCover(bmp);
        delete bmp;
      }
    }

    BString path;
    int32 i = 0;
    while (msg->FindString("file", i++, &path) == B_OK) {
      if (!path.IsEmpty())
        FetchEmbeddedCoverBitmap(path);
    }
  }

  fWindow->UpdateFileInfo();
}

/**
 * @brief Returns embedded cover bytes for metadata properties UI.
 */
void ArtworkController::RequestEmbeddedCover(BMessage *msg) {
  BString file;
  if (msg->FindString("file", &file) != B_OK || file.IsEmpty())
    return;

  CoverBlob cover;
  if (MetadataTagIO::ExtractEmbeddedCover(BPath(file.String()), cover)) {
    BMessage reply(MSG_PROP_SET_COVER_DATA);
    reply.AddData("bytes", B_RAW_TYPE, cover.data(), (ssize_t)cover.size());

    BMessenger sender = msg->ReturnAddress();
    sender.SendMessage(&reply);
  }
}

ArtworkController::~ArtworkController() {
  delete fOpenPanel;
  fOpenPanel = nullptr;
  for (auto &pair : fCddaCoverCache) {
    delete pair.second;
  }
  fCddaCoverCache.clear();
}

std::vector<BString> ArtworkController::GetMainCoverTargetPaths() const {
  std::vector<BString> paths;
  if (!fWindow)
    return paths;

  MediaTableView *cv =
      fWindow->fLibraryManager ? fWindow->fLibraryManager->ContentView() : nullptr;
  if (cv) {
    BRow *row = nullptr;
    while ((row = cv->CurrentSelection(row)) != nullptr) {
      int32 idx = cv->IndexOf(row);
      const MediaItem *mi = cv->ItemAt(idx);
      if (mi && !mi->path.IsEmpty())
        paths.push_back(mi->path);
    }
  }

  if (paths.empty() && fWindow->fPlaybackTransportController &&
      fWindow->fPlaybackTransportController->NowPlayingIsValid()) {
    const MediaItem *np = fWindow->fPlaybackTransportController->NowPlayingItem();
    if (np && !np->path.IsEmpty())
      paths.push_back(np->path);
  }

  if (paths.empty() && cv) {
    const MediaItem *sel = cv->SelectedItem();
    if (sel && !sel->path.IsEmpty())
      paths.push_back(sel->path);
  }

  return paths;
}

bool ArtworkController::CanModifyFiles(const std::vector<BString> &paths) const {
  if (paths.empty())
    return false;

  for (const auto &path : paths) {
    BEntry entry(path.String(), true);
    if (entry.InitCheck() != B_OK || !entry.Exists() ||
        access(path.String(), W_OK) != 0) {
      return false;
    }
  }
  return true;
}

bool ArtworkController::HasCover(const std::vector<BString> &paths) const {
  if (paths.empty())
    return false;

  CoverBlob cb;
  BPath p(paths[0].String());
  return MetadataTagIO::ExtractEmbeddedCover(p, cb) && cb.data() && cb.size() > 0;
}

void ArtworkController::ShowCoverContextMenu(BPoint screenWhere) {
  if (!fWindow)
    return;

  std::vector<BString> targetPaths = GetMainCoverTargetPaths();
  bool canEdit = CanModifyFiles(targetPaths);
  bool hasCover = HasCover(targetPaths);

  BPopUpMenu menu("coverContext", false, false);

  auto *load =
      new BMenuItem(B_TRANSLATE("Load Cover..."), new BMessage(MSG_COVER_LOAD));
  auto *remove =
      new BMenuItem(B_TRANSLATE("Remove Cover"), new BMessage(MSG_COVER_CLEAR));
  auto *addToAlbum = new BMenuItem(B_TRANSLATE("Add to Album"),
                                   new BMessage(MSG_COVER_APPLY_ALBUM));
  auto *removeFromAlbum = new BMenuItem(B_TRANSLATE("Remove from Album"),
                                        new BMessage(MSG_COVER_CLEAR_ALBUM));
  auto *fetchFromMb =
      new BMenuItem(B_TRANSLATE("Fetch from MusicBrainz"),
                    new BMessage(MSG_COVER_FETCH_MB));

  load->SetEnabled(canEdit);
  remove->SetEnabled(canEdit);
  addToAlbum->SetEnabled(canEdit && hasCover);
  removeFromAlbum->SetEnabled(canEdit);
  fetchFromMb->SetEnabled(canEdit);

  menu.AddItem(load);
  menu.AddItem(remove);
  menu.AddSeparatorItem();
  menu.AddItem(addToAlbum);
  menu.AddItem(removeFromAlbum);
  menu.AddSeparatorItem();
  menu.AddItem(fetchFromMb);

  menu.SetTargetForItems(fWindow);
  menu.Go(screenWhere, true, true);
}

void ArtworkController::OpenCoverFilePanel() {
  if (!fWindow)
    return;

  if (!fOpenPanel) {
    BMessage *msg = new BMessage(MSG_MAIN_COVER_LOAD_REF);
    fOpenPanel = new BFilePanel(B_OPEN_PANEL, new BMessenger(fWindow), nullptr,
                                B_FILE_NODE, false, msg);
  }
  fOpenPanel->Show();
}

void ArtworkController::HandleCoverChosen(const entry_ref &ref) {
  BFile f(&ref, B_READ_ONLY);
  off_t sz = 0;
  if (f.InitCheck() != B_OK || f.GetSize(&sz) != B_OK || sz <= 0)
    return;

  std::vector<uint8> buf((size_t)sz);
  ssize_t rd = f.Read(buf.data(), (size_t)sz);
  if (rd <= 0)
    return;

  std::vector<BString> paths = GetMainCoverTargetPaths();
  if (paths.empty())
    return;

  BMessage payload(MSG_COVER_DROPPED_APPLY_ALL);
  for (const auto &p : paths)
    payload.AddString("file", p);
  payload.AddData("bytes", B_RAW_TYPE, buf.data(), buf.size());

  ApplyDroppedCoverToAll(&payload);
}

void ArtworkController::HandleMainCoverClear() {
  std::vector<BString> paths = GetMainCoverTargetPaths();
  if (paths.empty())
    return;

  BMessage payload(MSG_COVER_DROPPED_APPLY_ALL);
  payload.AddBool("clear_cover", true);
  for (const auto &p : paths)
    payload.AddString("file", p);

  ApplyDroppedCoverToAll(&payload);
}

void ArtworkController::HandleMainCoverApplyAlbum() {
  std::vector<BString> paths = GetMainCoverTargetPaths();
  if (paths.empty())
    return;

  CoverBlob cb;
  BPath p(paths[0].String());
  if (MetadataTagIO::ExtractEmbeddedCover(p, cb) && cb.data() && cb.size() > 0) {
    BMessage payload(MSG_COVER_APPLY_ALBUM);
    payload.AddString("file", paths[0]);
    payload.AddData("bytes", B_RAW_TYPE, cb.data(), cb.size());
    ApplyAlbumCover(&payload);
  }
}

void ArtworkController::HandleMainCoverClearAlbum() {
  std::vector<BString> paths = GetMainCoverTargetPaths();
  if (paths.empty())
    return;

  BMessage payload(MSG_COVER_CLEAR_ALBUM);
  payload.AddString("file", paths[0]);
  ClearAlbumCover(&payload);
}
