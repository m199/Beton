/*
 * Copyright 2005-2026, Daniel Weber.
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "MainWindow.h"
#include "ArtworkController.h"
#include "MediaTableView.h"
#include "DLNAMessageHandler.h"
#include "DLNAViewController.h"
#if ENABLE_LOCAL_OUTPUT
#include "OutputViewController.h"
#endif
#include "DLNAService.h"
#include "Debug.h"
#include "MusicSourceManagerWindow.h"
#include "NowPlayingInfoPanel.h"
#include "LibraryMessageHandler.h"
#include "LibraryController.h"
#include "MusicBrainzMatcherWindow.h"
#include "MetadataMessageHandler.h"
#include "MusicSourceSettings.h"
#include "PlaylistNameDialog.h"
#include "PlaybackMessageHandler.h"
#include "PlaybackTransportController.h"
#include "PlaybackQueueManager.h"
#include "PlaylistMessageHandler.h"
#include "SmartPlaylistGeneratorWindow.h"
#include "PlaylistSidebarView.h"
#include "PlaylistLibrary.h"
#include "PlaylistEditController.h"
#include "PlaylistSelectionController.h"

#include "PropertiesController.h"
#include "MetadataPropertiesWindow.h"
#include "RadioMessageHandler.h"
#include "RadioStationController.h"
#include "PlaybackSeekBarView.h"
#include "SettingsController.h"
#include "UndoManager.h"
#include "StatusBarController.h"
#include "SyncMessageHandler.h"
#include "MetadataSyncConflictDialog.h"
#include "MetadataSyncController.h"
#include "MetadataTagIO.h"
#include "ViewMessageHandler.h"
#include "ViewStateController.h"

#include <AboutWindow.h>
#include <Alert.h>
#include <AppFileInfo.h>
#include <Autolock.h>
#include <Button.h>
#include <ColumnTypes.h>
#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <FilePanel.h>
#include <FindDirectory.h>
#include <GroupLayout.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <OS.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <Roster.h>
#include <ScrollView.h>
#include <Slider.h>
#include <StatusBar.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>
#include <View.h>
#include <algorithm>
#include <random>
#include <vector>

#include <Catalog.h>

#include <Application.h>
#include <IconUtils.h>
#include <Resources.h>

#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "MainWindow"

/** @name Player Button Icon Resource IDs */
///@{
static constexpr int32 ICON_PLAY_GRAY = 2001;
static constexpr int32 ICON_PAUSE_GRAY = 2003;
static constexpr int32 ICON_PREV = 2005;
static constexpr int32 ICON_NEXT = 2006;
static constexpr int32 ICON_SHUFFLE_GRAY = 2007;
static constexpr int32 ICON_SHUFFLE_COLOR = 2008;
static constexpr int32 ICON_STOP = 2009;
static constexpr int32 ICON_REPEAT_GRAY = 2010;
static constexpr int32 ICON_REPEAT_GREEN = 2011;
static constexpr int32 ICON_REPEAT_ORANGE = 2012;
static constexpr int32 ICON_MUTE_ON = 2013;
static constexpr int32 ICON_MUTE_OFF = 2014;
static constexpr int32 ICON_OUTPUT_DEVICE = 1005;
///@}

bool MainWindow::_HandleViewMessage(BMessage *msg) {
  return fViewMessageHandler && fViewMessageHandler->HandleMessage(msg);
}

bool MainWindow::_HandlePlaybackMessage(BMessage *msg) {
  return fPlaybackMessageHandler && fPlaybackMessageHandler->HandleMessage(msg);
}

bool MainWindow::_HandlePlaylistMessage(BMessage *msg) {
  return fPlaylistMessageHandler && fPlaylistMessageHandler->HandleMessage(msg);
}

bool MainWindow::_HandleRadioMessage(BMessage *msg) {
  return fRadioMessageHandler && fRadioMessageHandler->HandleMessage(msg);
}

bool MainWindow::_HandleDlnaMessage(BMessage *msg) {
  return fDlnaCommandHandler && fDlnaCommandHandler->HandleMessage(msg);
}

#if ENABLE_LOCAL_OUTPUT
bool MainWindow::_HandleLocalOutputMessage(BMessage *msg) {
  if (!fLocalOutputController || !msg)
    return false;

  switch (msg->what) {
  case MSG_SHOW_LOCAL_OUTPUT_MENU:
    fLocalOutputController->ShowLocalOutputMenu();
    return true;

  case MSG_LOCAL_OUTPUT_SELECTED:
    fLocalOutputController->SelectLocalOutput(msg);
    return true;

  case MSG_LOCAL_OUTPUT_REFRESH:
    fLocalOutputController->RefreshDevices();
    return true;

  case MSG_LOCAL_OUTPUT_REFRESH + 100: // Toggle local output button
    fLocalOutputController->ToggleLocalOutputButton();
    return true;

  case MSG_LOCAL_OUTPUT_REFRESH + 101: // Trigger settings save
    SaveSettings();
    return true;

  case MSG_CONFLICT_POLICY_CHANGED:
    fLocalOutputController->SetConflictPolicy(msg);
    return true;

  case MSG_FALLBACK_DEVICE_CHANGED:
    fLocalOutputController->SetFallbackDevice(msg);
    return true;
  }
  return false;
}
#endif

bool MainWindow::_HandleStatusAndSearchMessage(BMessage *msg) {
  return fStatusBarController && fStatusBarController->HandleMessage(msg);
}

bool MainWindow::_HandleSyncMessage(BMessage *msg) {
  return fSyncMessageHandler && fSyncMessageHandler->HandleMessage(msg);
}

bool MainWindow::_HandleMetadataMessage(BMessage *msg) {
  return fMetadataMessageHandler && fMetadataMessageHandler->HandleMessage(msg);
}

void MainWindow::_ShowAboutWindow() {
  BAboutWindow *about = new BAboutWindow("Beton", "application/x-vnd.Beton");

  // Take the version from the binary that is actually running.
  //
  // Left to itself, BAboutWindow resolves the version through
  // be_roster->FindApp(signature) (AboutView::_GetVersionFromSignature), which
  // consults the MIME database's app hint (META:PPATH) and only falls back to a
  // cross-volume query. Nothing validates that the hint still points at this
  // binary, so with another Beton on any mounted volume -- a second install, an
  // old build tree -- the About box reports THAT one's version. Observed on a
  // machine with a dev volume mounted: an installed 1.3.1 reported "Version 1.2"
  // from a stale build's app_version resource. GetAppInfo() gives us our own
  // entry_ref directly, with no signature lookup to go wrong.
  app_info info;
  if (be_app->GetAppInfo(&info) == B_OK) {
    BFile file(&info.ref, B_READ_ONLY);
    BAppFileInfo appFileInfo;
    version_info version;
    if (file.InitCheck() == B_OK && appFileInfo.SetTo(&file) == B_OK &&
        appFileInfo.GetVersionInfo(&version, B_APP_VERSION_KIND) == B_OK) {
      BString versionString;
      versionString << B_TRANSLATE("Version") << " " << version.major << "."
                    << version.middle << "." << version.minor;
      about->SetVersion(versionString.String());
    }
  }

  about->AddCopyright(2025, "Daniel Weber");
  about->AddDescription("A music library manager and player for Haiku.\n\n"
                        "Solid grey and cold\nYet it vibrates with the "
                        "sound\nConcrete sings today\n\n"
                        "Thanks to:\n"
                        "andimachovec\n"
                        "Begasus\n"
                        "dcbeckman\n"
                        "Humdinger\n"
                        "niFinx\n"
                        "zuMi\n\n"
                        "Icons by zuMi\n"
                        "https://hvif-store.art/\n\n"
                        "Licensed under the MIT License.");
  about->Show();
}

void MainWindow::_ShowMatcherTestWindow() {
  std::vector<BString> files = {"File1.mp3", "File2.mp3", "File3.mp3",
                                "File4.mp3"};
  std::vector<MusicBrainzMatchTrackInfo> tracks;
  MusicBrainzMatchTrackInfo t1;
  t1.index = 1;
  t1.name = "Test Track 1";
  t1.duration = "3:30";
  tracks.push_back(t1);

  MusicBrainzMatchTrackInfo t2;
  t2.index = 2;
  t2.name = "Test Track 2";
  t2.duration = "4:45";
  tracks.push_back(t2);

  MusicBrainzMatchTrackInfo t3;
  t3.index = 3;
  t3.name = "Test Track 3";
  t3.duration = "2:20";
  tracks.push_back(t3);

  std::vector<int> map(files.size(), -1);

  new MusicBrainzMatcherWindow(files, tracks, map, BMessenger(this));
}

bool MainWindow::_HandleAppCommandMessage(BMessage *msg) {
  switch (msg->what) {
  case B_ABOUT_REQUESTED: {
    _ShowAboutWindow();
    break;
  }

  case MSG_TEST_MODE: {
    _ShowMatcherTestWindow();
    break;
  }

  case MSG_MANAGE_DIRECTORIES: {
    fLibraryController->ShowDirectoryManager();
    break;
  }

  case MSG_SIDEBAR_FOLDERS:
    _SendSidebarFolders();
    break;

  case MSG_LIBRARY_REMOVE:
    // From the menu it means the selection; Redo sends the paths.
    if (msg->HasString("path"))
      fLibraryController->RemovePathsFromLibrary(msg);
    else
      fLibraryController->RemoveSelectedFromLibrary();
    break;

  case MSG_LIBRARY_RESTORE:
    fLibraryController->RestoreToLibrary(msg);
    break;

  case MSG_PLAYLIST_SAVED: {
    std::vector<BString> paths;
    BString path;
    for (int32 i = 0; msg->FindString("path", i, &path) == B_OK; i++)
      paths.push_back(path);
    fLibraryController->AddPlaylistTracks(paths);
    break;
  }

  case MSG_REMOVE_MISSING:
    fLibraryController->RemoveMissingTracks();
    break;

  case MSG_ADD_TO_LIBRARY:
    fLibraryController->ShowAddToLibraryPanel();
    break;

  case MSG_ADD_TO_LIBRARY_REFS:
    fLibraryController->AddRefsToLibrary(msg);
    break;

  case MSG_DROP_INTO_FOLDER:
    fLibraryController->DropIntoFolder(msg);
    break;

  case MSG_PASTE_INTO_FOLDER: {
    BString folder;
    if (msg->FindString("folder", &folder) == B_OK)
      fLibraryController->PasteIntoFolder(folder);
    break;
  }

  case MSG_PASTE_FILES: {
    // The menu shortcut took Alt+V from text fields; give it back.
    if (auto *text = dynamic_cast<BTextView *>(CurrentFocus())) {
      PostMessage(B_PASTE, text);
      break;
    }
    if (fIsFolderMode && fPlaylistLibrary)
      fLibraryController->PasteIntoFolder(
          fPlaylistLibrary->FolderPathForName(fCurrentPlaylistName));
    break;
  }

  case MSG_FOLDER_COPY_DONE:
    fLibraryController->FolderCopyDone(msg);
    break;

  case B_CONTROL_INVOKED: {
    _HandleControlInvoked(msg);
    break;
  }

  case MSG_REVEAL_IN_TRACKER: {
    fLibraryController->RevealInTracker(msg);
    break;
  }

  case MSG_UNDO: {
    MediaTableView *cv =
        fLibraryManager ? fLibraryManager->ContentView() : nullptr;
    if (cv && cv->HasActiveEditor()) {
      // Let the inline text editor handle its own typing undo.
      if (auto *tv = dynamic_cast<BTextView *>(cv->ActiveEditor()))
        PostMessage(B_UNDO, tv);
      else if (auto *tc = dynamic_cast<BTextControl *>(cv->ActiveEditor()))
        PostMessage(B_UNDO, tc->TextView());
      break;
    }
    if (fUndoManager)
      fUndoManager->Undo();
    break;
  }

  case MSG_REDO: {
    MediaTableView *cv =
        fLibraryManager ? fLibraryManager->ContentView() : nullptr;
    if (cv && cv->HasActiveEditor())
      break;
    if (fUndoManager)
      fUndoManager->Redo();
    break;
  }

  default:
    return false;
  }
  return true;
}

void MainWindow::MenusBeginning() {
  BWindow::MenusBeginning();
  if (fUndoItem)
    fUndoItem->SetEnabled(fUndoManager && fUndoManager->CanUndo());
  if (fRedoItem)
    fRedoItem->SetEnabled(fUndoManager && fUndoManager->CanRedo());
  if (fPasteItem)
    fPasteItem->SetEnabled(
        dynamic_cast<BTextView *>(CurrentFocus()) != nullptr ||
        (fIsFolderMode && LibraryController::ClipboardHasFiles()));
  // Read-only sources: switching Fast Edit on would make their rows editable.
  if (fFastEditItem)
    fFastEditItem->SetEnabled(!fIsRadioMode && !fIsDlnaMode && !fIsCDMode);
}

void MainWindow::_HandleControlInvoked(BMessage *msg) {
  void *source = nullptr;
  MediaTableView *cv = fLibraryManager->ContentView();
  if (msg->FindPointer("source", &source) == B_OK && source == cv)
    PostMessage(MSG_PLAY);
}

bool MainWindow::_HandleLibraryDataMessage(BMessage *msg) {
  return fLibraryMessageHandler && fLibraryMessageHandler->HandleMessage(msg);
}

#if ENABLE_DLNA_OUTPUT
void MainWindow::_SetOutputMenuVisible(bool visible) {
  if (!fSettingsMenu || !fOutputMenuItem)
    return;

  if (fBtnRenderer) {
    bool showRendererButton = visible && fShowRendererBtn;
    bool isRendererButtonHidden = fBtnRenderer->IsHidden();
    if (showRendererButton && isRendererButtonHidden)
      fBtnRenderer->Show();
    else if (!showRendererButton && !isRendererButtonHidden)
      fBtnRenderer->Hide();

    if (fBtnRenderer->Parent()) {
      fBtnRenderer->Parent()->InvalidateLayout(true);
      fBtnRenderer->Parent()->Relayout();
    }
    InvalidateLayout(true);
    Layout(true);
  }

  bool isVisible = fOutputMenuItem->Menu() == fSettingsMenu;
  if (visible == isVisible)
    return;

  if (visible) {
    int32 index = fSettingsMenu->CountItems();
    if (fToggleDlnaItem) {
      int32 dlnaIndex = fSettingsMenu->IndexOf(fToggleDlnaItem);
      if (dlnaIndex >= 0)
        index = dlnaIndex + 1;
    }
    fSettingsMenu->AddItem(fOutputMenuItem, index);
  } else {
    fSettingsMenu->RemoveItem(fOutputMenuItem);
  }
}
#endif

/**
 * @brief Loads a vector icon from application resources and renders it to a
 * bitmap.
 * @param id The resource ID of the icon.
 * @param size The desired size in pixels.
 * @return A new BBitmap containing the rendered icon, or nullptr on failure.
 */
static BBitmap *LoadIconFromResource(int32 id, float size) {
  if (!be_app || !be_app->AppResources())
    return nullptr;

  size_t len = 0;
  const void *data =
      be_app->AppResources()->LoadResource(B_VECTOR_ICON_TYPE, id, &len);
  if (!data || len == 0) {
    DEBUG_PRINT("Icon-ID %ld not found\n", (long)id);
    return nullptr;
  }

  BRect r(0, 0, size - 1, size - 1);
  auto *bmp = new BBitmap(r, 0, B_RGBA32);
  if (BIconUtils::GetVectorIcon(static_cast<const uint8 *>(data), len, bmp) !=
      B_OK) {
    delete bmp;
    DEBUG_PRINT("Icon-ID %ld: Decoding failed\n", (long)id);
    return nullptr;
  }
  return bmp;
}

MainWindow *gMainWindow = nullptr;

extern void AddItemToPlaylist(const BString &playlist, const BString &path);

class WindowClickFilter : public BMessageFilter {
public:
  explicit WindowClickFilter(MainWindow *window)
      : BMessageFilter(B_ANY_DELIVERY, B_ANY_SOURCE, B_MOUSE_DOWN),
        fWindow(window) {}

  filter_result Filter(BMessage *msg, BHandler **target) override {
    if (fWindow && msg->what == B_MOUSE_DOWN) {
      if (fWindow->fLibraryManager && fWindow->fLibraryManager->ContentView()) {
        MediaTableView *view = fWindow->fLibraryManager->ContentView();
        if (view->HasActiveEditor()) {
          BView *v = dynamic_cast<BView *>(*target);
          bool clickOnEditor = false;
          bool clickInTable = false;
          for (BView *p = v; p; p = p->Parent()) {
            if (p == view->ActiveEditor()) {
              clickOnEditor = true;
              break;
            }
            if (p == view)
              clickInTable = true;
          }
          if (!clickOnEditor && !clickInTable) {
            view->CommitCellEdit();
          }
        }
      }
    }
    return B_DISPATCH_MESSAGE;
  }

private:
  MainWindow *fWindow;
};

/**
 * @brief Constructs the Main Window of the application.
 *
 * Initializes the UI, managers (Playlist, Library, Cache), and playback
 * controller. Starts the initial cache load and status updates.
 */
MainWindow::MainWindow()
    : BWindow(BRect(100, 100, 400, 300), "Beton", B_DOCUMENT_WINDOW,
              B_QUIT_ON_WINDOW_CLOSE),
      fNewFilesCount(0), fMetadataPropertiesWindow(nullptr), fPlaybackEngine(nullptr),
      fUpdateRunner(nullptr) {

  MusicSourceSettings::InitCache();

  fPlaybackEngine = new AudioPlaybackEngine();
  fPlaybackEngine->SetTarget(BMessenger(this));

  fPlaybackEngine->SetVolume(1.0f);

  fPlaylistLibrary = new PlaylistLibrary(BMessenger(this));
  fRadioStationLibrary = new RadioStationLibrary(BMessenger(this));

  fDlnaManager = new DLNAService(BMessenger(this));
#if ENABLE_DLNA_OUTPUT
  fPlaybackEngine->SetRemoteOutputManagers(fDlnaManager, &fLocalServer);
#endif

  fMediaLibraryCache = new MediaLibraryCache(BMessenger(this));
  fMediaLibraryCache->Run();

  fLibraryManager = new LibraryBrowserController(BMessenger(this));
  fMetadataService = new MetadataService(BMessenger(this));
  fArtworkController = new ArtworkController(this);
  fMetadataMessageHandler = new MetadataMessageHandler(this);
  fPlaybackMessageHandler = new PlaybackMessageHandler(this);
  fPlaybackTransportController = new PlaybackTransportController(this);
  fPlaybackQueueManager = new PlaybackQueueManager(this);
  fPlaylistMessageHandler = new PlaylistMessageHandler(this);
  fPlaylistSelectionController = new PlaylistSelectionController(this);
  fPlaylistEditController = new PlaylistEditController(this);
  fPropertiesController = new PropertiesController(this);
  fLibraryController = new LibraryController(this);
  fRadioStationController = new RadioStationController(this);
  fRadioMessageHandler = new RadioMessageHandler(this);
  fDlnaController = new DLNAViewController(this);
#if ENABLE_LOCAL_OUTPUT
  fLocalOutputController = new OutputViewController(this);
#endif
  fDlnaCommandHandler = new DLNAMessageHandler(this);
  fSettingsController = new SettingsController(this);
  fStatusBarController = new StatusBarController(this);
  fLibraryMessageHandler = new LibraryMessageHandler(this);
  fViewStateController = new ViewStateController(this);
  fViewMessageHandler = new ViewMessageHandler(this);
  fSyncMessageHandler = new SyncMessageHandler(this);
  fMetadataSyncController = new MetadataSyncController(this);
  fUndoManager = new UndoManager(this);

  fNowPlayingInfoPanel = new NowPlayingInfoPanel();
  fStatusLabel = new BStringView("status", B_TRANSLATE("Loading..."));

  fSeekBarColor = ui_color(B_CONTROL_HIGHLIGHT_COLOR);
  fSelectionColor = ui_color(B_LIST_SELECTED_BACKGROUND_COLOR);

  _BuildUI();

  font_height fh;
  be_plain_font->GetHeight(&fh);
  float fontHeight = fh.ascent + fh.descent + fh.leading;
  float windowWidth = fontHeight * 70;
  float windowHeight = windowWidth / 1.618f;
  ResizeTo(windowWidth, windowHeight);
  CenterOnScreen();

  float minW = std::max(580.0f, fontHeight * 41.0f);
  float minH = std::max(510.0f, fontHeight * 36.0f);
  SetSizeLimits(minW, 32768, minH, 32768);

  BMessenger(fMediaLibraryCache).SendMessage(MSG_LOAD_CACHE);

  fMbClient = new MusicBrainzApiClient("beton-app@outlook.com");

  fStatusLabel->SetText(B_TRANSLATE("Loading Music Library..."));

  RegisterWithMediaLibraryCache();

  LoadSettings();
  // Before any scan the window asks for: the cache needs the sidebar's
  // folders to tell their tracks from the rest.
  _SendSidebarFolders();

  BMessage msg(MSG_INIT_LIBRARY);
  PostMessage(&msg);

#if ENABLE_DLNA_OUTPUT
  if (fDlnaController)
    fDlnaController->RebuildRendererMenu();
  fLocalServer.Start();
#endif
#if ENABLE_LOCAL_OUTPUT
  if (fLocalOutputController) {
    // _BuildUI() ran before LoadSettings(), so the button was laid out from
    // the default rather than the saved setting; apply it now that it's known.
    fLocalOutputController->ApplyButtonVisibility();
    fLocalOutputController->RebuildOutputMenu();
  }

  BString breadcrumbPath;
  BPath bPath;
  if (find_directory(B_USER_SETTINGS_DIRECTORY, &bPath) == B_OK) {
    bPath.Append("Beton");
    bPath.Append("output_breadcrumb");
    breadcrumbPath = bPath.Path();
  }
  if (!breadcrumbPath.IsEmpty() && BEntry(breadcrumbPath.String()).Exists()) {
    BAlert* alert = new BAlert("Recovery",
        B_TRANSLATE("Beton exited uncleanly while using direct local audio output. "
                    "The system mixer output might still be redirected to that device.\n\n"
                    "Do you want to restore system audio routing to its original settings?"),
        B_TRANSLATE("No, keep current"),
        B_TRANSLATE("Yes, restore settings"),
        nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
    alert->SetShortcut(0, B_ESCAPE);
    int32 response = alert->Go();
    if (response == 1) {
      AudioOutputManager mgr;
      mgr.RecoverFromBreadcrumb();
    } else {
      unlink(breadcrumbPath.String());
    }
  }
#endif

  AddCommonFilter(new WindowClickFilter(this));
  _InitCDVolumeMonitoring();
}

/**
 * @brief Destructor.
 *
 * Cleans up all allocated managers, runners, and the playback controller.
 * Saves current settings before exit.
 */
MainWindow::~MainWindow() {
  fShuttingDown.store(true, std::memory_order_relaxed);
  delete fCDRescanRunner;
  fCDRescanRunner = nullptr;
  delete fGainPrefetchRunner;
  fGainPrefetchRunner = nullptr;
  if (fRadioStationController)
    fRadioStationController->CancelQueuedPlay();
  if (fPlaybackEngine)
    fPlaybackEngine->Stop();

  if (fRadioStationController)
    fRadioStationController->WaitForPlayThread();

  _WaitForLaunchedThreads();

  SaveSettings();
  if (fPlaybackEngine) {
    fPlaybackEngine->Shutdown();
    delete fPlaybackEngine;
    fPlaybackEngine = nullptr;
  }
  if (fMediaLibraryCache) {
    fMediaLibraryCache->Lock();
    fMediaLibraryCache->Quit();
    fMediaLibraryCache = nullptr;
  }
  delete fUpdateRunner;
  delete fBatchRunner;
  delete fSettingsController;
  delete fStatusBarController;
  delete fLibraryManager;
  delete fPlaylistLibrary;
  delete fArtworkController;
  delete fMetadataMessageHandler;
  delete fPlaybackMessageHandler;
  delete fPlaybackTransportController;
  delete fPlaybackQueueManager;
  delete fPlaylistMessageHandler;
  delete fPlaylistSelectionController;
  delete fPlaylistEditController;
  delete fPropertiesController;
  delete fLibraryController;
  delete fViewStateController;
  delete fFilePanel;
  delete fRadioMessageHandler;
  delete fRadioStationController;
  delete fRadioStationLibrary;
  delete fDlnaCommandHandler;
  delete fDlnaController;
#if ENABLE_LOCAL_OUTPUT
  delete fLocalOutputController;
#endif
  delete fDlnaManager;
  delete fLibraryMessageHandler;
  delete fViewMessageHandler;
  delete fSyncMessageHandler;
  delete fMetadataSyncController;
  delete fUndoManager;
  delete fMetadataService;
  delete fMbClient;
  delete fSearchRunner;
  delete fViewsRefreshRunner;

  delete fIconPlay;
  delete fIconPause;
  delete fIconStop;
  delete fIconNext;
  delete fIconPrev;
  delete fIconShuffleOff;
  delete fIconShuffleOn;
  delete fIconRepeatOff;
  delete fIconRepeatAll;
  delete fIconRepeatOne;
#if ENABLE_DLNA_OUTPUT
  if (fOutputMenuItem && fOutputMenuItem->Menu() == nullptr)
    delete fOutputMenuItem;
  delete fIconRenderer;
#endif
#if ENABLE_LOCAL_OUTPUT
  delete fIconLocalOutput;
#endif
}

/**
 * @brief Builds the User Interface.
 *
 * Creates the menu bar, toolbar buttons, status bar, and the main split view
 * containing the sidebar (playlists/info) and the library browser.
 */
void MainWindow::_BuildUI() {
  const float kItemSpacing = 3.0f;
  const float kGroupSpacing = 8.0f;

  fMenuBar = new BMenuBar("menuBar");
  fMenuBar->SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));

  BMenu *fileMenu = new BMenu(B_TRANSLATE("File"));
  fileMenu->AddItem(new BMenuItem(B_TRANSLATE("Add to Library" B_UTF8_ELLIPSIS),
                                  new BMessage(MSG_ADD_TO_LIBRARY)));
  fileMenu->AddItem(new BMenuItem(B_TRANSLATE("Manage Music Folders"),
                                  new BMessage(MSG_MANAGE_DIRECTORIES)));
  fileMenu->AddItem(
      new BMenuItem(B_TRANSLATE("Rescan"), new BMessage(MSG_RESCAN_FULL)));

  fileMenu->AddItem(new BMenuItem(B_TRANSLATE("Remove Missing Tracks"),
                                  new BMessage(MSG_REMOVE_MISSING)));

  fileMenu->AddItem(new BMenuItem(B_TRANSLATE("Synchronize Metadata"),
                                  new BMessage(MSG_SYNC_SMART)));

  fileMenu->AddSeparatorItem();
  fileMenu->AddItem(
      new BMenuItem(B_TRANSLATE("Quit"), new BMessage(B_QUIT_REQUESTED), 'q'));
  fMenuBar->AddItem(fileMenu);

  BMenu *editMenu = new BMenu(B_TRANSLATE("Edit"));
  fUndoItem = new BMenuItem(B_TRANSLATE("Undo"), new BMessage(MSG_UNDO), 'Z');
  fRedoItem = new BMenuItem(B_TRANSLATE("Redo"), new BMessage(MSG_REDO), 'Z',
                            B_SHIFT_KEY);
  fUndoItem->SetEnabled(false);
  fRedoItem->SetEnabled(false);
  editMenu->AddItem(fUndoItem);
  editMenu->AddItem(fRedoItem);
  editMenu->AddSeparatorItem();
  // Takes over Alt+V, so text fields get it passed on (MSG_PASTE_FILES).
  fPasteItem = new BMenuItem(B_TRANSLATE("Paste"),
                             new BMessage(MSG_PASTE_FILES), 'V');
  editMenu->AddItem(fPasteItem);
  editMenu->AddSeparatorItem();
  fFastEditItem = new BMenuItem(B_TRANSLATE("Enable Fast Edit"),
                                new BMessage(MSG_TOGGLE_FAST_EDIT), 'E');
  fFastEditItem->SetMarked(fFastEditEnabled);
  editMenu->AddItem(fFastEditItem);
  fMenuBar->AddItem(editMenu);

  BMenu *playlistMenu = new BMenu(B_TRANSLATE("Playlists"));
  playlistMenu->AddItem(new BMenuItem(B_TRANSLATE("New Playlist"),
                                      new BMessage(MSG_NEW_PLAYLIST)));
  playlistMenu->AddItem(new BMenuItem(B_TRANSLATE("Generate New Playlist"),
                                      new BMessage(MSG_NEW_SMART_PLAYLIST)));
  playlistMenu->AddSeparatorItem();
  playlistMenu->AddItem(new BMenuItem(B_TRANSLATE("Set Playlist Folder"),
                                      new BMessage(MSG_SET_PLAYLIST_FOLDER)));
  fMenuBar->AddItem(playlistMenu);

  fSettingsMenu = new BMenu(B_TRANSLATE("Settings"));

#if ENABLE_DLNA_OUTPUT
  fOutputMenu = new BMenu(B_TRANSLATE("Output"));
  fOutputMenuItem = new BMenuItem(fOutputMenu);
  fShowRendererBtnItem = new BMenuItem(B_TRANSLATE("Show Button"),
                                       new BMessage(MSG_TOGGLE_RENDERER_BTN));
  fShowRendererBtnItem->SetMarked(fShowRendererBtn);
  fOutputMenu->AddItem(fShowRendererBtnItem);
  fOutputMenu->AddSeparatorItem();
#endif

  fToggleRadioItem = new BMenuItem(B_TRANSLATE("Enable Radio"),
                                   new BMessage(MSG_TOGGLE_RADIO));
  fToggleRadioItem->SetMarked(fRadioEnabled);
  fToggleDlnaItem =
      new BMenuItem(B_TRANSLATE("Enable DLNA"), new BMessage(MSG_TOGGLE_DLNA));
  fToggleDlnaItem->SetMarked(fDlnaEnabled);

  fSettingsMenu->AddItem(fToggleRadioItem);
  fSettingsMenu->AddItem(fToggleDlnaItem);
#if ENABLE_DLNA_OUTPUT
  _SetOutputMenuVisible(fDlnaEnabled);
#endif
#if ENABLE_LOCAL_OUTPUT
  fLocalOutputSettingsMenu = new BMenu(B_TRANSLATE("Output Device"));
  fLocalOutputSettingsMenuItem = new BMenuItem(fLocalOutputSettingsMenu);
  fSettingsMenu->AddItem(fLocalOutputSettingsMenuItem);
#endif

  // Values are AudioPlaybackEngine::ReplayGainMode.
  fReplayGainMenu = new BMenu(B_TRANSLATE("ReplayGain"));
  const struct {
    int32 mode;
    const char *label;
  } replayGainModes[] = {
      {AudioPlaybackEngine::kReplayGainAuto,
       B_TRANSLATE("Auto (album when played in order)")},
      {AudioPlaybackEngine::kReplayGainTrack, B_TRANSLATE("Track")},
      {AudioPlaybackEngine::kReplayGainAlbum, B_TRANSLATE("Album")},
      {AudioPlaybackEngine::kReplayGainOff, B_TRANSLATE("Off")},
  };
  for (const auto &m : replayGainModes) {
    BMessage *msg = new BMessage(MSG_REPLAYGAIN_MODE);
    msg->AddInt32("mode", m.mode);
    BMenuItem *item = new BMenuItem(m.label, msg);
    item->SetMarked(m.mode == AudioPlaybackEngine::kReplayGainAuto);
    fReplayGainMenu->AddItem(item);
  }
  fReplayGainMenu->AddSeparatorItem();
  fGainPrefetchItem =
      new BMenuItem(B_TRANSLATE("Measure tracks without gain while playing"),
                    new BMessage(MSG_GAIN_PREFETCH_TOGGLE));
  fGainPrefetchItem->SetMarked(fGainPrefetch);
  fReplayGainMenu->AddItem(fGainPrefetchItem);
  fGainWriteItem =
      new BMenuItem(B_TRANSLATE("Save measured gain in the files"),
                    new BMessage(MSG_GAIN_WRITE_TOGGLE));
  fGainWriteItem->SetMarked(fGainWriteFiles);
  fReplayGainMenu->AddItem(fGainWriteItem);
  fSettingsMenu->AddItem(fReplayGainMenu);
  fSettingsMenu->AddSeparatorItem();

  BMenu *sidebarMenu = new BMenu(B_TRANSLATE("Sidebar"));
  fViewCoverItem =
      new BMenuItem(B_TRANSLATE("Album Cover"), new BMessage(MSG_ARTWORK_ON));
  fViewInfoItem = new BMenuItem(B_TRANSLATE("File Information"),
                                new BMessage(MSG_FILEINFO_ON));
  fViewFiltersItem =
      new BMenuItem(B_TRANSLATE("Filters"), new BMessage(MSG_TOGGLE_FILTERS));
  fViewCoverItem->SetMarked(fShowCoverArt);
  fViewInfoItem->SetMarked(fShowFileInfo);

  bool currentFilters = false;
  if (fIsLibraryMode)
    currentFilters = fShowFiltersLibrary;
  else if (fIsRadioMode)
    currentFilters = fShowFiltersRadio;
  else if (fIsDlnaMode)
    currentFilters = fShowFiltersDlna;
  else
    currentFilters = fShowFiltersPlaylist;
  fViewFiltersItem->SetMarked(currentFilters);

  fSettingsMenu->AddItem(fViewFiltersItem);

  sidebarMenu->AddItem(fViewCoverItem);
  sidebarMenu->AddItem(fViewInfoItem);
  fSettingsMenu->AddItem(sidebarMenu);

  BMenu *selColorMenu = new BMenu(B_TRANSLATE("Selection Color"));
  fSelColorSystemItem = new BMenuItem(B_TRANSLATE("System Default"),
                                      new BMessage(MSG_SELECTION_COLOR_SYSTEM));
  fSelColorMatchItem = new BMenuItem(B_TRANSLATE("Match SeekBar"),
                                     new BMessage(MSG_SELECTION_COLOR_MATCH));
  fSelColorSystemItem->SetMarked(!fUseSeekBarColorForSelection);
  fSelColorMatchItem->SetMarked(fUseSeekBarColorForSelection);
  selColorMenu->AddItem(fSelColorSystemItem);
  selColorMenu->AddItem(fSelColorMatchItem);
  fSettingsMenu->AddItem(selColorMenu);

  BMenu *tooltipsMenu = new BMenu(B_TRANSLATE("Tooltips"));
  fTooltipsOnItem =
      new BMenuItem(B_TRANSLATE("On"), new BMessage(MSG_TOOLTIPS_ON));
  fTooltipsOffItem =
      new BMenuItem(B_TRANSLATE("Off"), new BMessage(MSG_TOOLTIPS_OFF));
  fTooltipsOnItem->SetMarked(true);
  tooltipsMenu->AddItem(fTooltipsOnItem);
  tooltipsMenu->AddItem(fTooltipsOffItem);
  fSettingsMenu->AddItem(tooltipsMenu);

  fMenuBar->AddItem(fSettingsMenu);

  BMenu *helpMenu = new BMenu(B_TRANSLATE("Help"));
  helpMenu->AddItem(new BMenuItem(B_TRANSLATE("About Beton" B_UTF8_ELLIPSIS),
                                  new BMessage(B_ABOUT_REQUESTED)));
  fMenuBar->AddItem(helpMenu);

  fSeekBar = new PlaybackSeekBarView("seekbar");

  font_height fh;
  be_plain_font->GetHeight(&fh);
  float fontHeight = fh.ascent + fh.descent + fh.leading;
  float barHeight = std::max(22.0f, fontHeight * 1.5f);

  fSeekBar->SetExplicitMinSize(BSize(fontHeight * 14, barHeight));
  fSeekBar->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, barHeight));

  fTitleView = new MarqueeTextView("titleView");
  fTitleView->SetText(B_TRANSLATE("No Title"));
  fTitleView->SetExplicitMinSize(BSize(10, barHeight));
  fTitleView->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, barHeight));

  fBtnPrev = new BButton("", new BMessage(MSG_PREV_BTN));
  fBtnPlayPause = new BButton("", new BMessage(MSG_PLAYPAUSE));
  fBtnStop = new BButton("", new BMessage(MSG_STOP));
  fBtnNext = new BButton("", new BMessage(MSG_PLAY_NEXT));
  fBtnShuffle = new BButton("", new BMessage(MSG_SHUFFLE_TOGGLE));
  fBtnRepeat = new BButton("", new BMessage(MSG_REPEAT_TOGGLE));

  float size = fontHeight * 1.8f;
  if (size < 24.0f)
    size = 24.0f;
  BSize buttonSize(size, size);

  float iconSize = size * 0.65f; // Icon is 65% of button size
  fIconPlay = LoadIconFromResource(ICON_PLAY_GRAY, iconSize);
  fIconPause = LoadIconFromResource(ICON_PAUSE_GRAY, iconSize);
  fIconStop = LoadIconFromResource(ICON_STOP, iconSize);
  fIconNext = LoadIconFromResource(ICON_NEXT, iconSize);
  fIconPrev = LoadIconFromResource(ICON_PREV, iconSize);
  fIconShuffleOff = LoadIconFromResource(ICON_SHUFFLE_GRAY, iconSize);
  fIconShuffleOn = LoadIconFromResource(ICON_SHUFFLE_COLOR, iconSize);
  fIconRepeatOff = LoadIconFromResource(ICON_REPEAT_GRAY, iconSize);
  fIconRepeatAll = LoadIconFromResource(ICON_REPEAT_GREEN, iconSize);
  fIconRepeatOne = LoadIconFromResource(ICON_REPEAT_ORANGE, iconSize);
  fIconMuteOn = LoadIconFromResource(ICON_MUTE_ON, iconSize);
  fIconMuteOff = LoadIconFromResource(ICON_MUTE_OFF, iconSize);

  fBtnMute = new IconButtonView("mute_icon", fIconMuteOn, new BMessage(MSG_MUTE_TOGGLE));
#if ENABLE_DLNA_OUTPUT
  fIconRenderer = LoadIconFromResource(ICON_OUTPUT_DEVICE, iconSize);
#endif

  if (fIconPrev)
    fBtnPrev->SetIcon(fIconPrev, 0);
  if (fIconPlay)
    fBtnPlayPause->SetIcon(fIconPlay, 0);
  if (fIconStop)
    fBtnStop->SetIcon(fIconStop, 0);
  if (fIconNext)
    fBtnNext->SetIcon(fIconNext, 0);
  if (fIconShuffleOff)
    fBtnShuffle->SetIcon(fIconShuffleOff, 0);
  if (fIconRepeatOff)
    fBtnRepeat->SetIcon(fIconRepeatOff, 0);
  fBtnPlayPause->SetLabel("");

  fBtnPrev->SetExplicitSize(buttonSize);
  fBtnPlayPause->SetExplicitSize(buttonSize);
  fBtnShuffle->SetExplicitSize(buttonSize);
  fBtnRepeat->SetExplicitSize(buttonSize);
  fBtnStop->SetExplicitSize(buttonSize);
  fBtnNext->SetExplicitSize(buttonSize);
  fBtnMute->SetExplicitSize(BSize(iconSize * 1.2f, size));

  if (fShowTooltips && fViewStateController) {
    fViewStateController->UpdateTooltips();
  }

  fVolumeSlider = new BSlider("volume", nullptr, nullptr, 0, 100, B_HORIZONTAL);
  fVolumeSlider->SetModificationMessage(new BMessage(MSG_VOLUME_CHANGED));
  fVolumeSlider->SetValue(100);
  fVolumeSlider->SetExplicitMinSize(BSize(fontHeight * 6, B_SIZE_UNSET));
  fVolumeSlider->SetExplicitMaxSize(BSize(fontHeight * 8, B_SIZE_UNSET));

  fSearchField =
      new BTextControl("search", "", "", new BMessage(MSG_SEARCH_MODIFY));
  fSearchField->SetModificationMessage(new BMessage(MSG_SEARCH_MODIFY));
  fSearchField->SetTarget(this);
  fSearchField->SetExplicitSize(BSize(fontHeight * 15, B_SIZE_UNSET));

  fDlnaServerMenu = new BPopUpMenu("");
  fDlnaServerField = new BMenuField("dlna_server", nullptr, fDlnaServerMenu);
  fDlnaServerField->SetExplicitMinSize(BSize(fontHeight * 10, B_SIZE_UNSET));
  fDlnaServerField->SetExplicitMaxSize(BSize(fontHeight * 16, B_SIZE_UNSET));
  fDlnaServerField->Hide();
#if ENABLE_DLNA_OUTPUT
  fRendererMenu = new BPopUpMenu("");
  BMessage *localeMsg = new BMessage(MSG_RENDERER_SELECTED);
  localeMsg->AddString("uuid", "");
  BMenuItem *localeItem = new BMenuItem(B_TRANSLATE("Locale"), localeMsg);
  localeItem->SetMarked(true);
  fRendererMenu->AddItem(localeItem);
  fRendererMenu->SetTargetForItems(this);

  fBtnRenderer = new BButton("", new BMessage(MSG_SHOW_RENDERER_MENU));
  if (fIconRenderer)
    fBtnRenderer->SetIcon(fIconRenderer, 0);
  fBtnRenderer->SetExplicitSize(buttonSize);
#endif

#if ENABLE_LOCAL_OUTPUT
  fLocalOutputMenu = new BPopUpMenu("");
  fLocalOutputMenu->SetTargetForItems(this);

  fBtnLocalOutput = new BButton("", new BMessage(MSG_SHOW_LOCAL_OUTPUT_MENU));
  fIconLocalOutput = LoadIconFromResource(ICON_OUTPUT_DEVICE, iconSize);
  if (fIconLocalOutput)
    fBtnLocalOutput->SetIcon(fIconLocalOutput, 0);
  fBtnLocalOutput->SetExplicitSize(buttonSize);
  // Visibility is applied from the saved setting after LoadSettings(), which
  // runs later than this; see OutputViewController::ApplyButtonVisibility().
#endif

  BScrollView *playlistScroll = new BScrollView(
      "playlist_scroll", fPlaylistLibrary->View(), B_WILL_DRAW, false, true);
  playlistScroll->SetExplicitMinSize(BSize(0, 0));

  BScrollView *genreScroll = new BScrollView(
      "genre_scroll", fLibraryManager->GenreView(), B_WILL_DRAW, false, true);
  BScrollView *artistScroll = new BScrollView(
      "artist_scroll", fLibraryManager->ArtistView(), B_WILL_DRAW, false, true);
  BScrollView *albumScroll = new BScrollView(
      "album_scroll", fLibraryManager->AlbumView(), B_WILL_DRAW, false, true);
  BScrollView *contentScroll =
      new BScrollView("content_scroll", fLibraryManager->ContentView(),
                      B_WILL_DRAW, false, false);
  contentScroll->SetBorder(B_NO_BORDER);
  contentScroll->SetExplicitMinSize(BSize(0, 0));

  genreScroll->SetExplicitMinSize(BSize(0, 0));
  artistScroll->SetExplicitMinSize(BSize(0, 0));
  albumScroll->SetExplicitMinSize(BSize(0, 0));

  BGroupView *sidebarGroup = new BGroupView(B_VERTICAL, 0);
  sidebarGroup->SetExplicitMinSize(BSize(fontHeight * 14, B_SIZE_UNSET));
  sidebarGroup->SetExplicitMaxSize(BSize(fontHeight * 14, B_SIZE_UNSET));

  BLayoutBuilder::Group<>(sidebarGroup)
      .Add(playlistScroll, 1.0f)
      .Add(fNowPlayingInfoPanel, 0.0f);

  fFilterGroup = new BGroupView(B_HORIZONTAL, kItemSpacing);
  BLayoutBuilder::Group<>(fFilterGroup)
      .Add(genreScroll, 1.0f)
      .Add(artistScroll, 1.0f)
      .Add(albumScroll, 1.0f)
      .End();

  BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
      .Add(fMenuBar)
      .AddGroup(B_VERTICAL, kGroupSpacing)
      .SetInsets(kGroupSpacing)

      .AddGroup(B_HORIZONTAL, kItemSpacing)
      .Add(fSeekBar)
      .Add(new BView("spacer", B_WILL_DRAW), 0.0f)
      .Add(fTitleView)
      .End()

      .AddGroup(B_HORIZONTAL, kItemSpacing)
      .Add(fBtnPrev)
      .Add(fBtnPlayPause)
      .Add(fBtnStop)
      .Add(fBtnNext)
      .Add(fBtnShuffle)
      .Add(fBtnRepeat)
      .AddStrut(kItemSpacing)
      .Add(fBtnMute)
      .Add(fVolumeSlider)
#if ENABLE_DLNA_OUTPUT
      .AddStrut(kItemSpacing)
      .Add(fBtnRenderer)
#endif
#if ENABLE_LOCAL_OUTPUT
      .AddStrut(kItemSpacing)
      .Add(fBtnLocalOutput)
#endif
      .AddStrut(kItemSpacing)
      .Add(fDlnaServerField)
      .AddGlue()
      .Add(fSearchField)
      .End()

      .AddSplit(B_HORIZONTAL, kGroupSpacing)

      .Add(sidebarGroup, 0.25f)

      .AddGroup(B_VERTICAL, kItemSpacing, 0.75f)

      .Add(fFilterGroup, 1.0f)
      .Add(contentScroll, 2.0f)
      .End()
      .End()

      .AddGroup(B_HORIZONTAL, 0)
      .Add(fStatusLabel)
      .AddGlue()
      .End()
      .End();
}

void MainWindow::WindowActivated(bool active) {
  BWindow::WindowActivated(active);
  if (!active) {
    if (fLibraryManager && fLibraryManager->ContentView()) {
      fLibraryManager->ContentView()->CommitCellEdit();
    }
  } else {
#if ENABLE_LOCAL_OUTPUT
    if (fLocalOutputController) {
      fLocalOutputController->RebuildOutputMenu();
    }
#endif
  }
}

/**
 * @brief Main message loop handler.
 *
 * Handles all application messages, including:
 * Playback control (Play, Pause, stop, Next, Prev)
 * Library updates & scanning
 * Playlist management
 * Metadata updates & MusicBrainz integration
 * UI selection changes
 *
 * @param msg The received message.
 */
void MainWindow::MessageReceived(BMessage *msg) {
  if (msg->what == B_NODE_MONITOR) {
    int32 opcode = 0;
    if (msg->FindInt32("opcode", &opcode) == B_OK) {
      if (opcode == B_DEVICE_MOUNTED || opcode == B_DEVICE_UNMOUNTED) {
        _CheckMountedCDs();
        return;
      }
      // Entries renamed in "/" (a CD's mount point) or on a CD (its tracks):
      // Haiku's online CD lookup renames both a few seconds after mounting.
      int32 device = -1;
      msg->FindInt32("device", &device);
      if ((opcode == B_ENTRY_MOVED || opcode == B_ENTRY_CREATED ||
           opcode == B_ENTRY_REMOVED) &&
          (device == fRootDevice ||
           fCDDevicePaths.find(device) != fCDDevicePaths.end())) {
        _ScheduleCDRescan();
        return;
      }
    }
  }
  if (msg->what == MSG_CD_RESCAN) {
    delete fCDRescanRunner;
    fCDRescanRunner = nullptr;
    _CheckMountedCDs();
    _ReloadShownCD();
    return;
  }

  if (_HandleViewMessage(msg))
    return;
  if (_HandlePlaybackMessage(msg))
    return;

  if (_HandlePlaylistMessage(msg))
    return;

  if (_HandleRadioMessage(msg))
    return;

  if (_HandleDlnaMessage(msg))
    return;

#if ENABLE_LOCAL_OUTPUT
  if (_HandleLocalOutputMessage(msg))
    return;
#endif

  if (_HandleStatusAndSearchMessage(msg))
    return;

  if (_HandleSyncMessage(msg))
    return;

  if (_HandleMetadataMessage(msg))
    return;

  if (_HandleAppCommandMessage(msg))
    return;

  if (_HandleLibraryDataMessage(msg))
    return;

  BWindow::MessageReceived(msg);
}

/**
 * @brief Helper to spawn and resume a background thread.
 *
 * @param name Name of the thread (for system monitor).
 * @param func Lambda function to execute in the thread.
 * @return thread_id The ID of the spawned thread, or error code.
 */
thread_id MainWindow::LaunchThread(const char *name,
                                   std::function<void()> &&func) {
  auto *funcPtr = new std::function<void()>(std::move(func));

  BAutolock lock(&fWorkerThreadsLock);
  if (fShuttingDown.load(std::memory_order_relaxed)) {
    delete funcPtr;
    return B_NOT_ALLOWED;
  }

  thread_id thread = spawn_thread(_ThreadEntry, name, B_NORMAL_PRIORITY, funcPtr);
  if (thread >= 0) {
    fWorkerThreads.push_back(thread);
    resume_thread(thread);
  } else {
    delete funcPtr;
  }
  return thread;
}

void MainWindow::_WaitForLaunchedThreads() {
  std::vector<thread_id> threads;
  {
    BAutolock lock(&fWorkerThreadsLock);
    threads.swap(fWorkerThreads);
  }

  thread_id current = find_thread(nullptr);
  for (thread_id thread : threads) {
    if (thread < 0 || thread == current)
      continue;
    status_t exit;
    wait_for_thread(thread, &exit);
  }
}

/**
 * @brief Static entry point for spawned C++ threads.
 */
status_t MainWindow::_ThreadEntry(void *data) {
  auto *func = static_cast<std::function<void()> *>(data);
  if (func) {
    (*func)();
    delete func;
  }
  return B_OK;
}

/**
 * @brief Triggers a refresh of the library views based on current filters.
 */
void MainWindow::UpdateFilteredViews(bool preserveScroll) {
  if (fLibraryManager) {
    const auto &items =
        (fIsRadioMode || fIsDlnaMode) ? fRadioItems :
        (fIsCDMode) ? fLibraryManager->ActiveItems() : fAllItems;
    BString context = "Library";
    if (fIsCDMode)
      context = "CD";
    else if (fIsRadioMode)
      context = "Radio";
    else if (fIsDlnaMode)
      context = "DLNA";
    else if (fIsFolderMode)
      context = fCurrentPlaylistName;
    else if (!fIsLibraryMode)
      context = fCurrentPlaylistName;

    fLibraryManager->UpdateFilteredViews(
        items, fIsLibraryMode || fIsRadioMode || fIsDlnaMode,
        context,
        fSearchField->Text(), preserveScroll, true, IsPlaylistSelected() || fIsCDMode);
    if (!fIsRadioMode && !fIsDlnaMode) {
      if (fStatusBarController)
        fStatusBarController->UpdateLibraryStatus();
      // Measure the new rows on screen without waiting for the next tick.
      // Posted, so it reads the rows once the list has been laid out.
      if (!preserveScroll) {
        BMessage tick(MSG_GAIN_PREFETCH_TICK);
        tick.AddBool("list_changed", true);
        PostMessage(&tick);
      }
    }
  }
}

/**
 * @brief Registers this window as a listener for MediaLibraryCache updates.
 */
void MainWindow::RegisterWithMediaLibraryCache() {
  BMessage reg(MSG_REGISTER_TARGET);
  reg.AddMessenger("target", BMessenger(this));
  BMessenger(fMediaLibraryCache).SendMessage(&reg);

  DEBUG_PRINT("registered as UI target at MediaLibraryCache\n");
}

/**
 * @brief Updates the "Info" side panel with details of the selected item.
 *
 * Prioritizes data from the MediaItem (which includes BFS attributes and tags).
 * Only reads technical audio properties from file if missing in
 * MediaItem.
 */
void MainWindow::UpdateFileInfo() {
  const MediaItem *mi = nullptr;
  bool isPlaying =
      fPlaybackEngine && (fPlaybackEngine->IsPlaying() || fPlaybackEngine->IsPaused());

  bool isRadio =
      isPlaying && fRadioStationController && fRadioStationController->HasActiveStation();

  if (isPlaying && !isRadio && fPlaybackTransportController &&
      fPlaybackTransportController->NowPlayingIsValid()) {
    mi = fPlaybackTransportController->NowPlayingItem();
  }

  if (!mi && (!isPlaying || !isRadio)) {
    if (fLibraryManager && fLibraryManager->ContentView()) {
      mi = fLibraryManager->ContentView()->SelectedItem();
    }
  }

  if (!mi && !isRadio) {
    if (fNowPlayingInfoPanel) {
      fNowPlayingInfoPanel->SetFileInfo("");
      fNowPlayingInfoPanel->SetTechInfo("");
    }
    return;
  }

  BString techStr;
  int32 bitrate = mi ? mi->bitrate : 0;
  int32 sampleRate = mi ? mi->sampleRate : 0;
  int32 channels = mi ? mi->channels : 0;

  if (bitrate == 0 && fPlaybackEngine) {
    bitrate = fPlaybackEngine->CurrentBitrate();
  }
  if (sampleRate == 0 && fPlaybackEngine) {
    sampleRate = fPlaybackEngine->CurrentSampleRate();
  }
  if (channels == 0 && fPlaybackEngine) {
    channels = fPlaybackEngine->CurrentChannels();
  }

  if (bitrate > 0) {
    techStr << bitrate << " kbps";
  }
  if (sampleRate > 0) {
    if (!techStr.IsEmpty()) techStr << " | ";
    techStr << (sampleRate / 1000.0f) << " kHz";
  }
  if (channels > 0) {
    if (!techStr.IsEmpty()) techStr << " | ";
    techStr << (channels == 1 ? "Mono" : (channels == 2 ? "Stereo" : "Surround"));
  }

  BString ext;
  if (mi && !mi->path.IsEmpty()) {
    int32 extIdx = mi->path.FindLast('.');
    if (extIdx != B_ERROR && extIdx < mi->path.Length() - 1) {
      mi->path.CopyInto(ext, extIdx + 1, mi->path.Length() - extIdx - 1);
      int32 queryIdx = ext.FindFirst('?');
      if (queryIdx != B_ERROR)
        ext.Truncate(queryIdx);
      ext.ToUpper();
    }
  } else if (isRadio) {
    ext = "STREAM";
  }

  if (!ext.IsEmpty()) {
    if (!techStr.IsEmpty()) techStr << " | ";
    techStr << ext;
  }

  if (fNowPlayingInfoPanel) {
    if (isRadio) {
      fNowPlayingInfoPanel->SetBoxLabel(B_TRANSLATE("Now Playing (Radio)"));
      BString artist = fRadioStationController->ActiveStreamArtist();
      BString title = fRadioStationController->ActiveStreamTrackTitle();
      BString album = fRadioStationController->ActiveStreamAlbum();

      if (artist.IsEmpty() && title.IsEmpty()) {
        artist =
            fRadioStationController->ActiveStreamTitle() == "(Fetching metadata...)"
                ? ""
                : fRadioStationController->ActiveStreamTitle();
        title = fRadioStationController->ActiveStationName();
      }
      if (artist.IsEmpty() && !title.IsEmpty())
        artist = fRadioStationController->ActiveStationName();
      if (album.IsEmpty())
        album = fRadioStationController->ActiveStationGenre();

      fNowPlayingInfoPanel->SetTags(artist, title, album);
    } else if (mi) {
      if (isPlaying) {
        if (fDlnaController && fDlnaController->CurrentPlayIndex() >= 0) {
          fNowPlayingInfoPanel->SetBoxLabel(B_TRANSLATE("Now Playing (DLNA)"));
        } else {
          fNowPlayingInfoPanel->SetBoxLabel(B_TRANSLATE("Now Playing"));
        }
      } else {
        fNowPlayingInfoPanel->SetBoxLabel(B_TRANSLATE("File Information"));
      }

      BString artist = mi->artist.IsEmpty() ? "-" : mi->artist;
      BString album = mi->album.IsEmpty() ? "-" : mi->album;
      BString title = mi->title.IsEmpty() ? "-" : mi->title;
      int32 year = mi->year;

      if (year > 0)
        album << " (" << year << ")";

      fNowPlayingInfoPanel->SetTags(artist, title, album);
    }

    fNowPlayingInfoPanel->SetTechInfo(techStr);
  }
}

/**
 * @brief Helper to get the full path of an item in the Content View.
 */
BString MainWindow::GetPathForContentItem(int index) {
  MediaTableView *cv = fLibraryManager->ContentView();
  if (!cv)
    return "";

  const MediaItem *item = cv->ItemAt(index);
  if (!item)
    return "";

  return item->path;
}

/**
 * @brief Retrieves list of playlist names from PlaylistLibrary.
 */
void MainWindow::GetPlaylistNames(BMessage &out, bool onlyWritable) const {
  if (fPlaylistLibrary) {
    fPlaylistLibrary->GetPlaylistNames(out, onlyWritable);
  }
}

/**
 * @brief Adds an entry to a playlist.
 */
void MainWindow::AddPlaylistEntry(const BString &playlistName,
                                  const BString &label,
                                  const BString &fullPath) {
  if (fPlaylistLibrary) {
    fPlaylistLibrary->AddPlaylistEntry(label, fullPath);
  }
}
/**
 * @brief Updates the status bar text.
 *
 * @param text The message to display.
 * @param isPermanent If false, the message will be reset after a timeout.
 */
void MainWindow::UpdateStatus(const BString &text, bool isPermanent) {
  if (fStatusBarController)
    fStatusBarController->UpdateStatus(text, isPermanent);
}

/**
 * @brief Saves current UI state (columns, playlist path, etc.) to settings
 * file.
 */
void MainWindow::SaveSettings() {
  if (fSettingsController)
    fSettingsController->SaveSettings();
}

/**
 * @brief Loads UI state from settings file.
 */
void MainWindow::SetReplayGainMode(int32 mode) {
  if (fPlaybackEngine)
    fPlaybackEngine->SetReplayGainMode(mode);
  if (fReplayGainMenu == nullptr)
    return;
  for (int32 i = 0; BMenuItem *item = fReplayGainMenu->ItemAt(i); i++) {
    // The mode items; the menu also holds the measuring check items.
    int32 itemMode;
    if (item->Message() != nullptr &&
        item->Message()->FindInt32("mode", &itemMode) == B_OK)
      item->SetMarked(itemMode == mode);
  }
}

void MainWindow::LoadSettings() {
  if (fSettingsController)
    fSettingsController->LoadSettings();
}

/**
 * @brief Calculates the luminance of a color (0.0 - 1.0).
 */
static float CalculateLuminance(rgb_color color) {
  return (0.299f * color.red + 0.587f * color.green + 0.114f * color.blue) /
         255.0f;
}

/**
 * @brief Applies custom colors to SeekBar and selection.
 */
void MainWindow::ApplyColors() {
  rgb_color panelBg = ui_color(B_PANEL_BACKGROUND_COLOR);

  if (fMenuBar) {
    fMenuBar->SetViewColor(panelBg);
    fMenuBar->SetLowColor(panelBg);
    fMenuBar->Invalidate();
  }

  if (fTitleView) {
    fTitleView->UpdateSystemColors();
  }

  if (fBtnMute) {
    fBtnMute->UpdateSystemColors();
  }

  if (fSeekBar) {
    float bgLuminance = CalculateLuminance(panelBg);

    rgb_color bg;
    rgb_color border;

    if (bgLuminance < 0.3f) {
      bg = tint_color(panelBg, 0.85f); // Slightly lighter than black, but dark enough ;)
      border = tint_color(bg, B_LIGHTEN_1_TINT);
    } else {
      bg = tint_color(panelBg, B_DARKEN_1_TINT);
      border = tint_color(bg, B_DARKEN_2_TINT);
    }

    if (fUseCustomSeekBarColor) {
      fSeekBar->SetColors(bg, fSeekBarColor, border);
    } else {
      fSeekBar->SetColors(bg, ui_color(B_CONTROL_HIGHLIGHT_COLOR), border);
    }

    if (fSearchField) {
      if (bgLuminance < 0.5f) {
        fSearchField->TextView()->SetViewColor(tint_color(panelBg, 0.80f));
        fSearchField->TextView()->SetLowColor(tint_color(panelBg, 0.80f));
        fSearchField->TextView()->SetHighColor((rgb_color){220, 220, 220, 255});
      } else {
        fSearchField->TextView()->SetViewColor(
            ui_color(B_DOCUMENT_BACKGROUND_COLOR));
        fSearchField->TextView()->SetLowColor(
            ui_color(B_DOCUMENT_BACKGROUND_COLOR));
        fSearchField->TextView()->SetHighColor(ui_color(B_DOCUMENT_TEXT_COLOR));
      }
      fSearchField->TextView()->Invalidate();
    }
  }

  rgb_color selColor =
      fUseSeekBarColorForSelection
          ? (fUseCustomSeekBarColor ? fSeekBarColor
                                    : ui_color(B_CONTROL_HIGHLIGHT_COLOR))
          : ui_color(B_LIST_SELECTED_BACKGROUND_COLOR);

  selColor.alpha = 255;

  if (fLibraryManager && fLibraryManager->ContentView()) {
    MediaTableView *cv = fLibraryManager->ContentView();
    cv->SetColor(B_COLOR_SELECTION, selColor);

    float luminance = CalculateLuminance(selColor);
    rgb_color selTextColor = luminance > 0.5f ? (rgb_color){0, 0, 0, 255}
                                              : (rgb_color){255, 255, 255, 255};
    cv->SetColor(B_COLOR_SELECTION_TEXT, selTextColor);
  }

  if (fLibraryManager) {
    if (fLibraryManager->GenreView())
      fLibraryManager->GenreView()->SetSelectionColor(selColor);
    if (fLibraryManager->ArtistView())
      fLibraryManager->ArtistView()->SetSelectionColor(selColor);
    if (fLibraryManager->AlbumView())
      fLibraryManager->AlbumView()->SetSelectionColor(selColor);
  }

  if (fPlaylistLibrary && fPlaylistLibrary->View()) {
    fPlaylistLibrary->View()->SetSelectionColor(selColor);
  }
}

void MainWindow::_SendSidebarFolders() {
  if (!fMediaLibraryCache || !fPlaylistLibrary)
    return;
  BMessage folders(MSG_SIDEBAR_FOLDERS);
  for (const BString &path : fPlaylistLibrary->FolderSourcePaths())
    folders.AddString("path", path);
  BMessenger(fMediaLibraryCache).SendMessage(&folders);
}

void MainWindow::_InitCDVolumeMonitoring() {
  // Also starts the gain prefetcher's timer: it looks for tracks to measure
  // every two seconds, which follows track changes and scrolling closely
  // enough without watching for either.
  BMessage tick(MSG_GAIN_PREFETCH_TICK);
  fGainPrefetchRunner =
      new BMessageRunner(BMessenger(this), &tick, 2000000);

  fVolumeRoster.StartWatching(BMessenger(this));
  // Mount points live in "/"; renaming a volume renames its mount point.
  BEntry root("/");
  node_ref rootRef;
  if (root.GetNodeRef(&rootRef) == B_OK) {
    fRootDevice = rootRef.device;
    watch_node(&rootRef, B_WATCH_DIRECTORY, BMessenger(this));
  }
  _CheckMountedCDs();
}

void MainWindow::_ScheduleCDRescan() {
  // Wait for a burst of renames (one per track) to finish.
  delete fCDRescanRunner;
  BMessage rescan(MSG_CD_RESCAN);
  fCDRescanRunner = new BMessageRunner(BMessenger(this), &rescan, 500000, 1);
}

void MainWindow::_ReloadShownCD() {
  if (!fIsCDMode || !fPlaylistLibrary || !fPlaylistLibrary->View())
    return;
  PlaylistSidebarView *sidebar = fPlaylistLibrary->View();
  const int32 selected = sidebar->CurrentSelection();
  if (selected >= 0 && sidebar->KindAt(selected) == PlaylistItemKind::CD)
    sidebar->SelectionChanged(selected);
}

void MainWindow::_CheckMountedCDs() {
  BVolumeRoster roster;
  BVolume vol;
  roster.Rewind();

  std::vector<CDItemInfo> mountedCDs;
  std::vector<dev_t> devices;

  while (roster.GetNextVolume(&vol) == B_OK) {
    fs_info info;
    if (fs_stat_dev(vol.Device(), &info) == B_OK && strcmp(info.fsh_name, "cdda") == 0) {
      BDirectory rootDir;
      if (vol.GetRootDirectory(&rootDir) == B_OK) {
        // Track files get renamed too once the disc has been looked up.
        node_ref discRef;
        if (rootDir.GetNodeRef(&discRef) == B_OK)
          watch_node(&discRef, B_WATCH_DIRECTORY, BMessenger(this));
        BEntry rootEntry;
        rootDir.GetEntry(&rootEntry);
        BPath rootPath;
        if (rootEntry.GetPath(&rootPath) == B_OK) {
          BString cdPath = rootPath.Path();
          BNode node(&rootEntry);

          char buf[256];
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

          char volName[B_FILE_NAME_LENGTH];
          memset(volName, 0, sizeof(volName));
          vol.GetName(volName);

          BString label;
          if (!artist.IsEmpty() && !album.IsEmpty()) {
            label << artist << " - " << album;
          } else if (!artist.IsEmpty()) {
            label << artist << " - Audio CD";
          } else if (!album.IsEmpty()) {
            label = album;
          } else if (volName[0] != '\0' && strcmp(volName, "Audio CD") != 0) {
            label = volName;
          } else {
            label = "Audio CD";
          }

          int count = 1;
          for (const auto &existing : mountedCDs) {
            BString prefix = label;
            prefix << " (";
            if (existing.label == label || existing.label.StartsWith(prefix)) {
              count++;
            }
          }
          if (count > 1) {
            label << " (" << count << ")";
          }

          mountedCDs.push_back({label, cdPath});
          devices.push_back(vol.Device());
        }
      }
    }
  }

  // A disc renamed since the last scan keeps its sidebar row (and stays
  // selected) instead of looking like one disc ejected and another inserted.
  std::map<dev_t, BString> paths;
  for (size_t i = 0; i < mountedCDs.size(); i++) {
    paths[devices[i]] = mountedCDs[i].path;
    auto old = fCDDevicePaths.find(devices[i]);
    if (old == fCDDevicePaths.end() || old->second == mountedCDs[i].path)
      continue;
    if (fPlaylistLibrary && fPlaylistLibrary->View())
      fPlaylistLibrary->View()->RenameCDPath(old->second, mountedCDs[i].path);
    if (fCDPath == old->second)
      fCDPath = mountedCDs[i].path;
  }
  fCDDevicePaths = paths;

  if (fPlaylistLibrary && fPlaylistLibrary->View()) {
    fPlaylistLibrary->View()->SyncCDItems(mountedCDs);
  }

  if (!mountedCDs.empty()) {
    // Keep pointing at the disc the user is actually on. A rescan is triggered
    // by any mount/unmount, so blindly taking mountedCDs[0] here would drag a
    // second-drive selection back to the first drive.
    bool currentStillMounted = false;
    for (const auto &cd : mountedCDs) {
      if (cd.path == fCDPath) {
        currentStillMounted = true;
        break;
      }
    }
    if (!currentStillMounted)
      fCDPath = mountedCDs[0].path;
  } else {
    if (!fCDPath.IsEmpty()) {
      if (fIsCDMode) {
        if (fPlaylistLibrary && fPlaylistLibrary->View()) {
          fPlaylistLibrary->View()->SelectByName("Library");
        }
      }
      fCDPath.Truncate(0);
    }
  }
}
