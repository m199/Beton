/*
 * Copyright 2005-2026, Daniel Weber.
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "MediaLibraryCache.h"
#include "DLNAService.h"
#include "LibraryBrowserController.h"
#include "MarqueeTextView.h"
#include "MediaItem.h"
#include "AudioPlaybackEngine.h"
#include "Messages.h"
#include "MetadataService.h"
#include "MusicBrainzApiClient.h"
#include "PlaylistLibrary.h"
#include "RadioStationLibrary.h"
#include "MetadataTagIO.h"
#include "LocalFileHttpServer.h"
#include "Config.h"

#include <Bitmap.h>
#include <Button.h>
#include <Locker.h>
#include <MenuBar.h>
#include <MenuField.h>
#include <MessageRunner.h>
#include <OS.h>
#include <Slider.h>
#include <String.h>
#include <StringView.h>
#include <TextControl.h>
#include <Window.h>
#include <VolumeRoster.h>
#include <Volume.h>
#include <NodeMonitor.h>
#include <fs_info.h>
#include "IconButtonView.h"
#include <atomic>
#include <functional>
#include <map>
#include <vector>

class BFilePanel;
class PlaybackSeekBarView;
class NowPlayingInfoPanel;
class BGroupView;
class ArtworkController;
class DLNAMessageHandler;
class DLNAViewController;
class OutputViewController;
class LibraryMessageHandler;
class LibraryController;
class MetadataMessageHandler;
class MusicBrainzLookupController;
class PlaybackMessageHandler;
class PlaybackTransportController;
class PlaybackQueueManager;
class PlaylistMessageHandler;
class PlaylistSelectionController;
class PlaylistEditController;
class PropertiesController;
class MetadataPropertiesWindow;
class RadioMessageHandler;
class RadioStationController;
class SettingsController;
class StatusBarController;
class SyncMessageHandler;
class MetadataSyncConflictDialog;
class MetadataSyncController;
class ViewMessageHandler;
class ViewStateController;
class UndoManager;
enum class PlaylistItemKind;

/**
 * @class MainWindow
 * @brief The primary application window.
 *
 * Orchestrates the interaction between the UI components and the backend logic:
 * - **Managers:** Owns `LibraryBrowserController` (browsing), `PlaylistLibrary`
 * (playlists), and `MediaLibraryCache` (data).
 * - **Playback:** Owns and communicates with `AudioPlaybackEngine`.
 * - **Metadata:** Coordinates `MetadataService` and `MusicBrainzApiClient` for
 * tagging.
 * - **UI:** Creates and lays out standard Haiku widgets (BButton, BSlider,
 * BListView, etc.).
 *
 * It handles the main BMessage loop for user actions and background worker
 * notifications.
 */
class MainWindow : public BWindow {
public:
  MainWindow();
  virtual ~MainWindow();

  void MessageReceived(BMessage *msg) override;
  void WindowActivated(bool active) override;
  void MenusBeginning() override;

  /** @name Helpers used by child windows/components */
  ///@{
  BString GetPathForContentItem(int index);
  void GetPlaylistNames(BMessage &out, bool onlyWritable = true) const;
  void AddPlaylistEntry(const BString &playlistName, const BString &label,
                        const BString &fullPath);

  ///@}

  /** @name Core Update Logic */
  ///@{
  void UpdateFilteredViews(bool preserveScroll = false);
  void UpdateFileInfo();
  void UpdateStatus(const BString &text, bool isPermanent = false);
  /// True only for user playlists (not Library/Radio/DLNA/CD sources).
  bool IsPlaylistSelected() const {
    return !fIsLibraryMode && !fIsFolderMode && !fIsRadioMode && !fIsDlnaMode && !fIsCDMode;
  }
  bool IsFolderMode() const { return fIsFolderMode; }
  bool IsRadioMode() const { return fIsRadioMode; }
  bool IsDlnaMode() const { return fIsDlnaMode; }
  bool IsCDMode() const { return fIsCDMode; }

  ///@}

  /** @name Settings */
  ///@{
  void LoadSettings();
  void SaveSettings();
  /** Sets the ReplayGain playback mode and marks it in the Settings menu. */
  void SetReplayGainMode(int32 mode);

  ///@}

  /** @name Async Logic */
  ///@{
  void RegisterWithMediaLibraryCache();
  thread_id LaunchThread(const char *name, std::function<void()> &&func);

  ///@}

private:
  friend class WindowClickFilter;
  friend class ArtworkController;
  friend class DLNAMessageHandler;
  friend class DLNAViewController;
  friend class OutputViewController;
  friend class LibraryMessageHandler;
  friend class LibraryController;
  friend class MetadataMessageHandler;
  friend class MusicBrainzLookupController;
  friend class PlaybackMessageHandler;
  friend class PlaybackTransportController;
  friend class PlaybackQueueManager;
  friend class PlaylistMessageHandler;
  friend class PlaylistSelectionController;
  friend class PlaylistEditController;
  friend class PropertiesController;
  friend class RadioMessageHandler;
  friend class RadioStationController;
  friend class SettingsController;
  friend class StatusBarController;
  friend class SyncMessageHandler;
  friend class MetadataSyncController;
  friend class ViewMessageHandler;
  friend class ViewStateController;

  static status_t _ThreadEntry(void *data);

  void _BuildUI();
  bool _HandleViewMessage(BMessage* msg);
  bool _HandlePlaybackMessage(BMessage* msg);
  bool _HandlePlaylistMessage(BMessage* msg);
  bool _HandleRadioMessage(BMessage* msg);
  bool _HandleDlnaMessage(BMessage* msg);
#if ENABLE_LOCAL_OUTPUT
  bool _HandleLocalOutputMessage(BMessage* msg);
#endif
  bool _HandleStatusAndSearchMessage(BMessage* msg);
  bool _HandleSyncMessage(BMessage* msg);
  bool _HandleMetadataMessage(BMessage* msg);
  bool _HandleAppCommandMessage(BMessage* msg);
  bool _HandleLibraryDataMessage(BMessage* msg);
  void _WaitForLaunchedThreads();

  void _HandleControlInvoked(BMessage* msg);
  void _ShowAboutWindow();
  void _ShowMatcherTestWindow();
#if ENABLE_DLNA_OUTPUT
  void _SetOutputMenuVisible(bool visible);
#endif
  /** Sends the sidebar's folders to the library cache. */
  void _SendSidebarFolders();
  void _InitCDVolumeMonitoring();
  void _CheckMountedCDs();
  /** Rescans CDs shortly after a rename, once the renames settle. */
  void _ScheduleCDRescan();
  /** Reloads the track list if a CD is being shown. */
  void _ReloadShownCD();


  /** @name Data & State */
  ///@{
  std::vector<MediaItem> fAllItems;   ///< Complete database cache
  std::vector<MediaItem> fRadioItems; ///< Radio stations as MediaItems
  bool fIsLibraryMode = true; ///< True = All tracks, False = Playlist view
  bool fIsFolderMode = false; ///< True = live folder source view
  bool fIsRadioMode = false;  ///< True = Radio station view
  bool fIsDlnaMode = false;   ///< True = DLNA server browsing view
  bool fIsCDMode = false;     ///< True = Audio CD source view
  BString fCDPath;
  BMessenger fCDRipWindow; ///< The open "Rip audio CD" window, if any.
  BVolumeRoster fVolumeRoster;
  /** Mounted CDs by device: a CD looked up online is renamed after mounting,
   *  so its path changes while the device stays the same. */
  std::map<dev_t, BString> fCDDevicePaths;
  dev_t fRootDevice = -1;                     ///< Where mount points live.
  BMessageRunner *fCDRescanRunner = nullptr;
  BString fPlaylistPath;
  BString fCurrentPlaylistName;

  bool fShowFiltersLibrary = true;
  bool fShowFiltersPlaylist = true;
  bool fShowFiltersRadio = false;
  bool fShowFiltersDlna = false;
  DLNADevice fActiveDlnaServer; ///< Currently browsed DLNA server
  bool fDlnaCrawling = false;  ///< True while a BrowseAll crawl is running
  
  BString fInitialViewMode;
  BString fInitialPlaylistName;
  BString fInitialDlnaUuid;

  std::atomic<int32> fMbSearchGeneration{0}; ///< Generation counter to invalidate old async searches

  ///@}

  /** @name MusicBrainz & Metadata Context */
  ///@{
  MBRelease fPendingRelease;
  CoverBlob fPendingCoverBlob;
  std::vector<BString> fPendingFiles;

  ///@}

  /** @name Cache Loading State */
  ///@{
  std::vector<MediaItem> fPendingItems;
  std::vector<BString> fPendingPlaylistOrder;
  std::map<BString, size_t> fPathIndex; ///< Maps file path to index in fAllItems for O(log n) lookup
  int32 fCurrentIndex{0};
  int32 fNewFilesCount{0};
  bool fCacheLoaded = false;

  ///@}

  /** @name Playback State */
  ///@{
  BString fLastSelectedPath;
  std::atomic<bool> fShuttingDown{false};
  BLocker fWorkerThreadsLock;
  std::vector<thread_id> fWorkerThreads;

  ///@}

  /** @name UI Components */
  ///@{
  BMenuBar *fMenuBar;
  BMenu *fSettingsMenu = nullptr;
  BMenu *fReplayGainMenu = nullptr;
  BButton *fBtnPrev;
  BButton *fBtnPlayPause;
  BButton *fBtnStop;
  BButton *fBtnNext;
  BButton *fBtnShuffle;
  BButton *fBtnRepeat;
  IconButtonView *fBtnMute;
  BSlider *fVolumeSlider;
  bool fIsMuted{false};
  float fPreMuteVolume{100.0f};
  bigtime_t fLastUserVolumeChange{0};
#if ENABLE_DLNA_OUTPUT
  BButton *fBtnRenderer{nullptr};
  BPopUpMenu *fRendererMenu{nullptr};
#endif
#if ENABLE_LOCAL_OUTPUT
  BButton *fBtnLocalOutput{nullptr};
  BPopUpMenu *fLocalOutputMenu{nullptr};
  BMenu *fLocalOutputSettingsMenu{nullptr};
  BMenuItem *fLocalOutputSettingsMenuItem{nullptr};
  bool fShowLocalOutputBtn{true};
#endif
  BMenuField *fDlnaServerField{nullptr};
  BPopUpMenu *fDlnaServerMenu{nullptr};

  BTextControl *fSearchField;

  BStringView *fStatusLabel;
  MarqueeTextView *fTitleView;
  PlaybackSeekBarView *fSeekBar;

  bool fShowCoverArt = true;
  bool fShowFileInfo = true;
  BMenuItem *fViewInfoItem = nullptr;
  BMenuItem *fViewCoverItem = nullptr;
  BMenuItem *fViewFiltersItem = nullptr;
  NowPlayingInfoPanel *fNowPlayingInfoPanel = nullptr;
  BGroupView *fFilterGroup = nullptr;
  bool fIsFilterGroupVisible = true;

  bool fShowTooltips = true;
  BMenuItem *fTooltipsOnItem = nullptr;
  BMenuItem *fTooltipsOffItem = nullptr;

  bool fFastEditEnabled = false;
  /// Measure tracks without ReplayGain while playing (GainPrefetcher).
  bool fGainPrefetch = true;
  /// Save gain measured while playing in the files, not just the library.
  bool fGainWriteFiles = false;
  BMenuItem *fGainPrefetchItem = nullptr;
  BMenuItem *fGainWriteItem = nullptr;
  BMessageRunner *fGainPrefetchRunner = nullptr;
  BMenuItem *fFastEditItem = nullptr;

  UndoManager *fUndoManager{nullptr};
  BMenuItem *fUndoItem = nullptr;
  BMenuItem *fRedoItem = nullptr;
  BMenuItem *fPasteItem = nullptr;

  bool fRadioEnabled = false;
  bool fDlnaEnabled = false;
#if ENABLE_DLNA_OUTPUT
  bool fShowRendererBtn = false;
#endif
  BMenuItem *fToggleRadioItem = nullptr;
  BMenuItem *fToggleDlnaItem = nullptr;
#if ENABLE_DLNA_OUTPUT
  BMenuItem *fShowRendererBtnItem = nullptr;
  BMenu *fOutputMenu = nullptr;
  BMenuItem *fOutputMenuItem = nullptr;
#endif

  ///@}

  /** @name Player Icon Bitmaps */
  ///@{
  BBitmap *fIconPlay{nullptr};
  BBitmap *fIconPause{nullptr};
  BBitmap *fIconStop{nullptr};
  BBitmap *fIconNext{nullptr};
  BBitmap *fIconPrev{nullptr};
  BBitmap *fIconShuffleOff{nullptr};
  BBitmap *fIconShuffleOn{nullptr};
  BBitmap *fIconRepeatOff{nullptr};
  BBitmap *fIconRepeatAll{nullptr};
  BBitmap *fIconRepeatOne{nullptr};
  BBitmap *fIconMuteOn{nullptr};
  BBitmap *fIconMuteOff{nullptr};
#if ENABLE_DLNA_OUTPUT
  BBitmap *fIconRenderer{nullptr};
#endif
#if ENABLE_LOCAL_OUTPUT
  BBitmap *fIconLocalOutput{nullptr};
#endif

  ///@}

  /** @name Color Customization */
  ///@{
  rgb_color fSeekBarColor;
  rgb_color fSelectionColor;
  bool fUseCustomSeekBarColor = false;
  bool fUseSeekBarColorForSelection = false;
  void ApplyColors();
  BMenuItem *fSelColorSystemItem = nullptr;
  BMenuItem *fSelColorMatchItem = nullptr;

  ///@}

  /** @name Child Windows */
  ///@{
  MetadataPropertiesWindow *fMetadataPropertiesWindow{nullptr};
  BFilePanel *fFilePanel{nullptr};

  ///@}

  /** @name Managers & Controllers */
  ///@{
  LibraryBrowserController *fLibraryManager;
  PlaylistLibrary *fPlaylistLibrary;
  MetadataService *fMetadataService;

  MediaLibraryCache *fMediaLibraryCache;
  ArtworkController *fArtworkController{nullptr};
  LibraryMessageHandler *fLibraryMessageHandler{nullptr};
  LibraryController *fLibraryController{nullptr};
  MusicBrainzApiClient *fMbClient;
  AudioPlaybackEngine *fPlaybackEngine;
  MetadataMessageHandler *fMetadataMessageHandler{nullptr};
  PlaybackMessageHandler *fPlaybackMessageHandler{nullptr};
  PlaybackTransportController *fPlaybackTransportController{nullptr};
  PlaybackQueueManager *fPlaybackQueueManager{nullptr};
  PlaylistMessageHandler *fPlaylistMessageHandler{nullptr};
  PlaylistSelectionController *fPlaylistSelectionController{nullptr};
  PlaylistEditController *fPlaylistEditController{nullptr};
  PropertiesController *fPropertiesController{nullptr};
  RadioStationLibrary *fRadioStationLibrary;
  RadioMessageHandler *fRadioMessageHandler{nullptr};
  RadioStationController *fRadioStationController{nullptr};
  SettingsController *fSettingsController{nullptr};
  StatusBarController *fStatusBarController{nullptr};
  SyncMessageHandler *fSyncMessageHandler{nullptr};
  MetadataSyncController *fMetadataSyncController{nullptr};
  DLNAService *fDlnaManager;
  DLNAMessageHandler *fDlnaCommandHandler{nullptr};
  DLNAViewController *fDlnaController{nullptr};
#if ENABLE_LOCAL_OUTPUT
  OutputViewController *fLocalOutputController{nullptr};
#endif
  ViewMessageHandler *fViewMessageHandler{nullptr};
  ViewStateController *fViewStateController{nullptr};
  LocalFileHttpServer fLocalServer;

  static const uint32 MSG_RENDERER_SELECTED = 'rndS';
  static const uint32 MSG_SHOW_RENDERER_MENU = 'shRM';
  static const uint32 MSG_TOGGLE_RADIO = 'tgRd';
  static const uint32 MSG_TOGGLE_DLNA = 'tgDl';
  static const uint32 MSG_TOGGLE_RENDERER_BTN = 'tgRB';
#if ENABLE_LOCAL_OUTPUT
  static const uint32 MSG_SHOW_LOCAL_OUTPUT_MENU = 'shLO';
  static const uint32 MSG_TOGGLE_LOCAL_OUTPUT_BTN = 'tgLO';
#endif
  ///@}

  /** @name Message Runners (Timers) */
  ///@{
  BMessageRunner *fBatchRunner{nullptr};  ///< Slow-loading UI batch timer
  BMessageRunner *fUpdateRunner{nullptr}; ///< Playback progress update timer
  BMessageRunner *fStatusRunner{nullptr}; ///< Status bar clear timer
  BMessageRunner *fSearchRunner{nullptr}; ///< Search debounce timer
  BMessageRunner *fViewsRefreshRunner{nullptr}; ///< View refresh debounce timer
  ///@}
};

#endif // MAINWINDOW_H
