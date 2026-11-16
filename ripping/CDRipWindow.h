/*
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#ifndef BETON_CD_RIP_WINDOW_H
#define BETON_CD_RIP_WINDOW_H

#include "CDRipSettings.h"
#include "MediaItem.h"

#include <Messenger.h>
#include <String.h>
#include <Window.h>

#include <vector>

class BButton;
class BCheckBox;
class BColumn;
class BColumnListView;
class BFilePanel;
class BMenuField;
class BMessageRunner;
class BPopUpMenu;
class BStatusBar;
class BStringView;
class BTextControl;
class CDRipper;

/**
 * @class CDRipWindow
 * @brief Lets the user pick a format, folder and naming pattern, then rips
 *        the whole disc with CDRipper and shows each track's AccurateRip
 *        result.
 *
 * Opened from the sidebar's CD row. When a rip completes and "Add tracks to
 * library" is on, the new files are sent to @p libraryTarget as
 * MSG_CD_RIP_IMPORT to be added to the library. Double-clicking a ripped track sends the
 * ripped files to the same target as MSG_CD_RIP_PLAY.
 */
class CDRipWindow : public BWindow {
public:
  CDRipWindow(const BString &discPath, const BString &discLabel,
              BMessenger libraryTarget);
  ~CDRipWindow() override;

  void MessageReceived(BMessage *msg) override;
  bool QuitRequested() override;

private:
  void _BuildUI();
  void _LoadTracks();
  void _ApplySettings();
  void _ReadControls();
  RipFormat _SelectedFormat() const;
  bool _ParseOffset(int32 &offset, bool &known) const;
  /** The give-up field: 0-10, or -1 (never) for anything else. */
  int32 _ParseGiveUp() const;
  /** Shows a ripping track's unreadable sectors so far in Read errors. */
  void _ShowLiveUnreadable(int32 track, int32 sectors);
  void _UpdateDriveInfo();
  void _SetTrackStatus(int32 index, const char *text);
  /** Colors a track's row: a TrackResult from CDRipWindow.cpp. */
  void _SetTrackResult(int32 index, int32 result);
  /** Whether track @p index has a ripped file that still exists. */
  bool _HasRippedFile(int32 index) const;
  /** The right-click menu of track @p index, at screen point @p where. */
  void _ShowTrackMenu(int32 index, BPoint where);
  /** Moves track @p index's ripped file to the Trash, via the main window. */
  void _TrashRippedFile(int32 index);
  /** Shows how long track @p index has been ripping in Elapsed. */
  void _ShowTrackElapsed(int32 index);
  /** The progress bar's trailing text: percentage and total time so far. */
  void _ShowTotalElapsed();
  bool _IsTrackChecked(int32 index) const;
  int32 _CountCheckedTracks() const;
  void _CheckAllTracks(bool checked);
  /** Shows all, none or mixed in the "All tracks" box. */
  void _UpdateAllCheck();

  void _UpdatePreview();
  void _ChooseFolder();
  void _StartRip();
  void _SetRipping(bool ripping);
  void _HandleProgress(BMessage *msg);
  void _HandleTrackDone(BMessage *msg);
  void _HandleFinished(BMessage *msg);
  void _HandleOffset(BMessage *msg);
  void _ShowOutputFolder();
  /** Plays the ripped files, starting at the selected track. */
  void _PlaySelectedTrack();

  BString fDiscPath;
  BString fDiscLabel;
  BMessenger fLibraryTarget;
  std::vector<MediaItem> fTracks;
  std::vector<BString> fRippedPaths; ///< Per track; empty until it is ripped.
  bigtime_t fLastToggle = 0;         ///< When a checkbox was last clicked.
  CDRipSettings fSettings;

  CDRipper *fRipper = nullptr;
  BFilePanel *fFolderPanel = nullptr;
  std::vector<BString> fFailures;
  BString fOutputFolder; ///< Folder of the first file written by the last rip.
  BString fDriveName;    ///< "vendor model", the key for the saved offset.
  BString fDetectedOffset; ///< Offset text the last rip detected, if any.
  bool fDetectedFromList = false; ///< ...taken from AccurateRip's drive list.
  int32 fCurrentTrack = -1;
  int32 fCurrentPercent = -1;

  // Rip timing: per track from its first progress message to its result,
  // and the whole job from pressing Rip.
  std::vector<bigtime_t> fTrackStarted; ///< Per track; 0 = not started.
  int32 fTimingTrack = -1;              ///< Track whose clock is running.
  bigtime_t fRipStarted = 0;            ///< 0 when no rip is running.
  BString fPercentText;                 ///< The progress bar's "45%".
  BMessageRunner *fTicker = nullptr;    ///< kMsgTick while ripping.

  BStringView *fDiscInfo = nullptr;
  BColumnListView *fTrackList = nullptr;
  BColumn *fCheckColumn = nullptr; ///< The CheckColumn from CDRipWindow.cpp.
  BCheckBox *fAllCheck = nullptr;
  BTextControl *fOffsetControl = nullptr;
  BStringView *fDriveInfo = nullptr;
  BCheckBox *fVerifyCheck = nullptr;
  BCheckBox *fLibraryCheck = nullptr;
  BPopUpMenu *fFormatMenu = nullptr;
  BMenuField *fFormatField = nullptr;
  BTextControl *fFolderControl = nullptr;
  BButton *fChooseButton = nullptr;
  BTextControl *fPatternControl = nullptr;
  BMenuField *fPresetField = nullptr;
  BStringView *fPreview = nullptr;
  BStatusBar *fProgress = nullptr;
  BButton *fShowButton = nullptr;
  BButton *fCloseButton = nullptr;
  BButton *fRipButton = nullptr;
  BButton *fSkipButton = nullptr;
  BTextControl *fGiveUpControl = nullptr;
};

#endif // BETON_CD_RIP_WINDOW_H
