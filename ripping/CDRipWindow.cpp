/*
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

/**
 * @file CDRipWindow.cpp
 * @brief The "Rip CD" window: format, folder and naming, then progress.
 */

#include "CDRipWindow.h"

#include "AccurateRip.h"
#include "CDRipper.h"
#include "CddaTracks.h"
#include "Messages.h"
#include "RipNamePattern.h"

#include <Alert.h>
#include <Button.h>
#include <Catalog.h>
#include <CheckBox.h>
#include <ColumnListView.h>
#include <ColumnTypes.h>
#include <ControlLook.h>
#include <Directory.h>
#include <Entry.h>
#include <FilePanel.h>
#include <FindDirectory.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <Node.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <Roster.h>
#include <StatusBar.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>
#include <Volume.h>
#include <fs_info.h>

#include <algorithm>
#include <stdlib.h>
#include <string.h>

#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "CDRipWindow"

namespace {

enum {
  kMsgFormatChanged = 'rfmt',
  kMsgFolderEdited = 'rfed',
  kMsgChooseFolder = 'rfch',
  kMsgFolderChosen = 'rfcd',
  kMsgPatternEdited = 'rped',
  kMsgPreset = 'rprs',
  kMsgRip = 'rrip',
  kMsgClose = 'rcls',
  kMsgShowFolder = 'rshw',
  kMsgOffsetEdited = 'roed',
  kMsgVerifyChanged = 'rver',
  kMsgToggleAll = 'rtal',
  kMsgTrackToggled = 'rttg',
  kMsgPlayTrack = 'rply',
  kMsgTrackMenu = 'rtmn',  ///< Right click on a track: "index", "where".
  kMsgTrashTrack = 'rttr', ///< Move a track's ripped file to the Trash.
  kMsgSkipTrack = 'rskp',
  kMsgGiveUpEdited = 'rgue',
  kMsgTick = 'rtik', ///< Once a second while ripping: the running times.
  kMsgEditCell = 'redc',  ///< A click asks to edit a cell: "index", "column".
  kMsgStartEdit = 'rsed', ///< ...after the double-click time, unless played.
  kMsgEditDone = 'redd',  ///< From the editor: "commit", "move", "editor".
  kMsgAlbumEdited = 'raed',    ///< Album fields typed in.
  kMsgAlbumCommitted = 'raec'  ///< Album field left: save to the disc.
};

/** How a finished track came out; picks its text color in the track list. */
enum TrackResult : int32 {
  kResultNone = 0, ///< Not ripped yet, skipped, stopped, or nothing checked.
  kResultClean,    ///< No read errors left, but no AccurateRip match.
  kResultVerified, ///< No read errors left, or an AccurateRip match.
  kResultMinor,    ///< Errors left that are likely inaudible.
  kResultNoticeable, ///< Errors left that may be heard as clicks.
  kResultBad       ///< Errors most people will hear, or the rip failed.
};

/** Whether a track is to be ripped. */
class CheckField : public BField {
public:
  bool checked = true;
};

/**
 * A checkbox per track. Toggles on click (unless locked during a rip) and
 * tells the window, which keeps the "All tracks" box in step.
 *
 * Haiku's OutlineView looks the clicked field up by the column's display
 * position, not its field number, so this column has to be field 0 and the
 * list must not let columns be moved.
 */
class CheckColumn : public BColumn {
public:
  CheckColumn(float width, BMessenger target)
      : BColumn(width, width, width, B_ALIGN_CENTER), fTarget(target) {
    SetWantsEvents(true);
  }

  void SetLocked(bool locked) { fLocked = locked; }

  void GetColumnName(BString *into) const override {
    *into = B_TRANSLATE("Rip");
  }
  void DrawTitle(BRect, BView *) override {}

  void DrawField(BField *field, BRect rect, BView *parent) override {
    CheckField *f = dynamic_cast<CheckField *>(field);
    if (f == nullptr)
      return;
    const float size = std::min(14.0f, rect.Height() - 4);
    BRect box(0, 0, size, size);
    box.OffsetTo(rect.left + floorf((rect.Width() - size) / 2),
                 rect.top + floorf((rect.Height() - size) / 2));
    uint32 flags = f->checked ? BControlLook::B_ACTIVATED : 0;
    if (fLocked)
      flags |= BControlLook::B_DISABLED;
    be_control_look->DrawCheckBox(parent, box, box,
                                  ui_color(B_CONTROL_BACKGROUND_COLOR), flags);
  }

  int CompareFields(BField *a, BField *b) override {
    return (int)static_cast<CheckField *>(a)->checked -
           (int)static_cast<CheckField *>(b)->checked;
  }

  bool AcceptsField(const BField *field) const override {
    return dynamic_cast<const CheckField *>(field) != nullptr;
  }

  void MouseDown(BColumnListView *parent, BRow *row, BField *field, BRect,
                 BPoint, uint32) override {
    CheckField *f = dynamic_cast<CheckField *>(field);
    if (fLocked || f == nullptr)
      return;
    f->checked = !f->checked;
    parent->UpdateRow(row);
    fTarget.SendMessage(kMsgTrackToggled);
  }

private:
  BMessenger fTarget;
  bool fLocked = false;
};

/** A track-list cell that remembers its row's result. */
class TrackField : public BStringField {
public:
  explicit TrackField(const char *text) : BStringField(text) {}
  TrackResult result = kResultNone;
};

/**
 * Draws a cell in its row's result color. Shades depend on the list's
 * background so they stay readable in light and dark themes.
 */
class TrackColumn : public BStringColumn {
public:
  TrackColumn(const char *title, float width, float minWidth, float maxWidth,
              uint32 truncate, alignment align = B_ALIGN_LEFT)
      : BStringColumn(title, width, minWidth, maxWidth, truncate, align) {
    // For the right-click menu. OutlineView still selects the row and
    // treats a double click as before.
    SetWantsEvents(true);
  }

  /** Makes a click on the selected row's cell edit it (kColTitle...). */
  void SetEditColumn(int32 column) { fEditColumn = column; }

  void MouseDown(BColumnListView *parent, BRow *row, BField *, BRect,
                 BPoint where, uint32 buttons) override {
    if (row == nullptr)
      return;
    if ((buttons & B_SECONDARY_MOUSE_BUTTON) == 0) {
      // Called before the list changes the selection: a click on the row
      // that was already selected, like a rename in Tracker. The window
      // waits out the double-click time, as a double click plays.
      int32 clicks = 1;
      if (BMessage *current = parent->Window()->CurrentMessage())
        current->FindInt32("clicks", &clicks);
      if (fEditColumn >= 0 && row->IsSelected() && clicks == 1) {
        BMessage edit(kMsgEditCell);
        edit.AddInt32("index", parent->IndexOf(row));
        edit.AddInt32("column", fEditColumn);
        parent->Window()->PostMessage(&edit);
      }
      return;
    }
    // Opened by the window once this click is handled, so the menu doesn't
    // run inside the list's own mouse tracking.
    BMessage menu(kMsgTrackMenu);
    menu.AddInt32("index", parent->IndexOf(row));
    parent->ScrollView()->ConvertToScreen(&where);
    menu.AddPoint("where", where);
    parent->Window()->PostMessage(&menu);
  }

  void DrawField(BField *field, BRect rect, BView *parent) override {
    TrackField *f = dynamic_cast<TrackField *>(field);
    if (f == nullptr || f->result == kResultNone) {
      BStringColumn::DrawField(field, rect, parent);
      return;
    }
    const rgb_color bg = ui_color(B_LIST_BACKGROUND_COLOR);
    const bool dark = bg.red * 0.299f + bg.green * 0.587f + bg.blue * 0.114f < 128;
    rgb_color color;
    switch (f->result) {
    case kResultVerified:
      color = dark ? make_color(80, 235, 80) : make_color(0, 140, 0);
      break;
    case kResultClean:
      color = dark ? make_color(165, 205, 165) : make_color(90, 140, 90);
      break;
    case kResultMinor:
      color = dark ? make_color(235, 215, 80) : make_color(150, 120, 0);
      break;
    case kResultNoticeable:
      color = dark ? make_color(255, 165, 60) : make_color(205, 100, 0);
      break;
    default:
      color = dark ? make_color(255, 95, 95) : make_color(200, 0, 0);
      break;
    }
    const rgb_color old = parent->HighColor();
    parent->SetHighColor(color);
    BStringColumn::DrawField(field, rect, parent);
    parent->SetHighColor(old);
  }

private:
  int32 fEditColumn = -1;
};


enum {
  kColCheck, // Must stay field 0: see CheckColumn.
  kColNumber,
  kColTitle,
  kColArtist,
  kColLength,
  kColGain,
  kColErrors,
  kColElapsed,
  kColStatus
};

const float kProgressMax = 1000.0f;

struct Preset {
  const char *label;
  const char *pattern;
};

const Preset kPresets[] = {
    {B_TRANSLATE_MARK("Album artist/Album/01 - Title"),
     "{albumartist}/{album}/{track} - {title}"},
    {B_TRANSLATE_MARK("Album artist/Year - Album/01 - Title"),
     "{albumartist}/{year} - {album}/{track} - {title}"},
    {B_TRANSLATE_MARK("Album artist - Album/01 - Title"),
     "{albumartist} - {album}/{track} - {title}"},
    {B_TRANSLATE_MARK("Album/01 - Artist - Title"),
     "{album}/{track} - {artist} - {title}"},
    {B_TRANSLATE_MARK("01 - Artist - Title (no folders)"),
     "{track} - {artist} - {title}"},
};

BString FormatDuration(int32 seconds) {
  BString s;
  if (seconds >= 3600)
    s.SetToFormat("%ld:%02ld:%02ld", (long)(seconds / 3600),
                  (long)(seconds / 60 % 60), (long)(seconds % 60));
  else
    s.SetToFormat("%ld:%02ld", (long)(seconds / 60), (long)(seconds % 60));
  return s;
}

} // namespace

/**
 * The text field laid over a Title or Artist cell while it is edited.
 * Enter saves, Escape cancels, Tab and Shift-Tab save and go on to the next
 * or previous cell; clicking elsewhere saves. Tells the window once, with
 * kMsgEditDone; the window removes it.
 */
class CellEditor : public BTextView {
public:
  CellEditor(BRect frame, const char *text, BMessenger target)
      : BTextView(frame, "cell editor",
                  BRect(3, 1, frame.Width() - 3, frame.Height() - 1),
                  B_FOLLOW_NONE, B_WILL_DRAW | B_NAVIGABLE),
        fTarget(target) {
    SetWordWrap(false);
    DisallowChar('\n');
    SetViewUIColor(B_DOCUMENT_BACKGROUND_COLOR);
    SetText(text);
  }

  /** The window is done with this editor: no more kMsgEditDone. */
  void Finish() { fDone = true; }

  void KeyDown(const char *bytes, int32 count) override {
    switch (bytes[0]) {
    case B_ENTER:
      _Done(true, 0);
      return;
    case B_ESCAPE:
      _Done(false, 0);
      return;
    case B_TAB:
      _Done(true, (modifiers() & B_SHIFT_KEY) != 0 ? -1 : 1);
      return;
    }
    BTextView::KeyDown(bytes, count);
  }

  void MakeFocus(bool focus) override {
    BTextView::MakeFocus(focus);
    if (!focus)
      _Done(true, 0);
  }

  void Draw(BRect update) override {
    BTextView::Draw(update);
    const rgb_color old = HighColor();
    SetHighColor(ui_color(B_KEYBOARD_NAVIGATION_COLOR));
    StrokeRect(Bounds());
    SetHighColor(old);
  }

private:
  void _Done(bool commit, int32 move) {
    if (fDone)
      return;
    fDone = true;
    BMessage done(kMsgEditDone);
    done.AddBool("commit", commit);
    done.AddInt32("move", move);
    done.AddPointer("editor", this);
    fTarget.SendMessage(&done);
  }

  BMessenger fTarget;
  bool fDone = false;
};

CDRipWindow::CDRipWindow(const BString &discPath, const BString &discLabel,
                         BMessenger libraryTarget)
    : BWindow(BRect(0, 0, 800, 300), B_TRANSLATE("Rip audio CD"),
              B_TITLED_WINDOW,
              B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS |
                  B_ASYNCHRONOUS_CONTROLS | B_CLOSE_ON_ESCAPE),
      fDiscPath(discPath), fDiscLabel(discLabel), fLibraryTarget(libraryTarget) {
  fSettings.Load();
  fDriveName = AccurateRip::DriveName(fDiscPath);
  _LoadTracks();
  _BuildUI();
  _ApplySettings();
  _SetRipping(false);
  _UpdatePreview();

  ResizeToPreferred();
  if (Bounds().Width() < 800)
    ResizeTo(800, Bounds().Height());
  CenterOnScreen();
}

CDRipWindow::~CDRipWindow() {
  delete fTicker;
  delete fEditDelay;
  delete fRipper;
  delete fFolderPanel;
}

void CDRipWindow::_LoadTracks() {
  fTracks = CddaTracks::Read(fDiscPath);
  fTitleFromFile.assign(fTracks.size(), false);
  for (size_t i = 0; i < fTracks.size(); i++) {
    MediaItem &t = fTracks[i];
    t.trackTotal = (int32)fTracks.size();
    // Untitled discs fall back to the file name ("01 Audio Track.wav");
    // drop the extension so it doesn't end up inside the new file name.
    BPath p(t.path.String());
    if (p.InitCheck() == B_OK && t.title == p.Leaf()) {
      fTitleFromFile[i] = true;
      int32 dot = t.title.FindLast('.');
      if (dot > 0)
        t.title.Truncate(dot);
    }
  }
  if (!fTracks.empty())
    fAlbumArtist = fTracks.front().albumArtist;
}

void CDRipWindow::_UpdateDiscInfo() {
  int32 seconds = 0;
  for (const MediaItem &t : fTracks)
    seconds += t.duration;
  BString info(fDiscLabel);
  BString detail;
  detail.SetToFormat(B_TRANSLATE("%ld tracks, %s"), (long)fTracks.size(),
                     FormatDuration(seconds).String());
  info << "  (" << detail << ")";
  fDiscInfo->SetText(info.String());
}

void CDRipWindow::_BuildUI() {
  fDiscInfo = new BStringView("disc", "");
  _UpdateDiscInfo();
  fDiscInfo->SetFont(be_bold_font);
  fDiscInfo->SetTruncation(B_TRUNCATE_END);
  fDiscInfo->SetExplicitMinSize(BSize(200, B_SIZE_UNSET));
  fDiscInfo->SetExplicitPreferredSize(BSize(400, B_SIZE_UNSET));
  fDiscInfo->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

  fTrackList = new BColumnListView("tracks", B_NAVIGABLE, B_FANCY_BORDER, false);
  fTrackList->SetSelectionMode(B_SINGLE_SELECTION_LIST);
  fTrackList->SetColor(B_COLOR_BACKGROUND, ui_color(B_LIST_BACKGROUND_COLOR));
  fTrackList->SetColor(B_COLOR_TEXT, ui_color(B_LIST_ITEM_TEXT_COLOR));
  fTrackList->SetColor(B_COLOR_SELECTION,
                       ui_color(B_LIST_SELECTED_BACKGROUND_COLOR));
  fTrackList->SetColor(B_COLOR_SELECTION_TEXT,
                       ui_color(B_LIST_SELECTED_ITEM_TEXT_COLOR));
  fTrackList->SetColor(B_COLOR_ROW_DIVIDER, B_TRANSPARENT_COLOR);
  fTrackList->SetColor(B_COLOR_HEADER_BACKGROUND,
                       ui_color(B_PANEL_BACKGROUND_COLOR));
  fTrackList->SetColor(B_COLOR_HEADER_TEXT, ui_color(B_PANEL_TEXT_COLOR));
  // Resizing only: moving columns would break the checkbox column's clicks.
  fTrackList->SetColumnFlags(B_ALLOW_COLUMN_RESIZE);
  fCheckColumn = new CheckColumn(24, BMessenger(this));
  fTrackList->AddColumn(fCheckColumn, kColCheck);
  fTrackList->AddColumn(new TrackColumn("#", 30, 26, 50, B_TRUNCATE_END,
                                          B_ALIGN_RIGHT), kColNumber);
  TrackColumn *titleColumn = new TrackColumn(B_TRANSLATE("Title"), 170, 60,
                                             500, B_TRUNCATE_END);
  titleColumn->SetEditColumn(kColTitle);
  fTrackList->AddColumn(titleColumn, kColTitle);
  TrackColumn *artistColumn = new TrackColumn(B_TRANSLATE("Artist"), 125, 50,
                                              400, B_TRUNCATE_END);
  artistColumn->SetEditColumn(kColArtist);
  fTrackList->AddColumn(artistColumn, kColArtist);
  fTrackList->AddColumn(new TrackColumn(B_TRANSLATE("Length"), 52, 40, 80,
                                          B_TRUNCATE_END, B_ALIGN_RIGHT),
                        kColLength);
  fTrackList->AddColumn(new TrackColumn(B_TRANSLATE("Gain"), 62, 40, 90,
                                          B_TRUNCATE_END, B_ALIGN_RIGHT),
                        kColGain);
  // The two message columns may grow as wide as their longest message (a
  // grade, glitch count and loudest spot didn't fit in 300 pixels,
  // 2026-11-12); the list scrolls sideways when they outgrow the window.
  fTrackList->AddColumn(new TrackColumn(B_TRANSLATE("Read errors"), 110, 50,
                                          1200, B_TRUNCATE_END), kColErrors);
  // How long each track took (or has taken so far) to rip.
  fTrackList->AddColumn(new TrackColumn(B_TRANSLATE("Elapsed"), 62, 40, 100,
                                          B_TRUNCATE_END, B_ALIGN_RIGHT),
                        kColElapsed);
  fTrackList->AddColumn(new TrackColumn(B_TRANSLATE("Status"), 195, 60, 1200,
                                          B_TRUNCATE_END), kColStatus);
  for (const MediaItem &t : fTracks) {
    BRow *row = new BRow();
    BString number;
    number << t.track;
    row->SetField(new CheckField(), kColCheck);
    row->SetField(new TrackField(number.String()), kColNumber);
    row->SetField(new TrackField(t.title.String()), kColTitle);
    row->SetField(new TrackField(t.artist.String()), kColArtist);
    row->SetField(new TrackField(FormatDuration(t.duration).String()),
                  kColLength);
    row->SetField(new TrackField(""), kColGain);
    row->SetField(new TrackField(""), kColErrors);
    row->SetField(new TrackField(""), kColElapsed);
    row->SetField(new TrackField(""), kColStatus);
    fTrackList->AddRow(row);
  }
  fTrackList->SetExplicitMinSize(BSize(B_SIZE_UNSET, 180));
  fTrackList->SetInvocationMessage(new BMessage(kMsgPlayTrack));
  fRippedPaths.assign(fTracks.size(), BString());

  // The album: what all tracks share. A field typed in changes the tracks
  // at once (the preview follows); leaving it saves it to the disc.
  BString album, albumArtist, year, genre;
  if (!fTracks.empty()) {
    album = fTracks.front().album;
    albumArtist = fTracks.front().albumArtist;
    if (fTracks.front().year > 0)
      year << fTracks.front().year;
    genre = fTracks.front().genre;
  }
  fAlbumArtistControl =
      new BTextControl("album_artist", B_TRANSLATE("Album artist:"),
                       albumArtist.String(), new BMessage(kMsgAlbumCommitted));
  fAlbumArtistControl->SetModificationMessage(new BMessage(kMsgAlbumEdited));
  fAlbumControl = new BTextControl("album", B_TRANSLATE("Album:"),
                                   album.String(),
                                   new BMessage(kMsgAlbumCommitted));
  fAlbumControl->SetModificationMessage(new BMessage(kMsgAlbumEdited));
  fYearControl = new BTextControl("year", B_TRANSLATE("Year:"), year.String(),
                                  new BMessage(kMsgAlbumCommitted));
  fYearControl->SetModificationMessage(new BMessage(kMsgAlbumEdited));
  {
    // Digits only, four of them.
    BTextView *text = fYearControl->TextView();
    for (uint32 c = 0; c < 256; c++) {
      if (c < '0' || c > '9')
        text->DisallowChar(c);
    }
    text->SetMaxBytes(4);
  }
  fGenreControl = new BTextControl("genre", B_TRANSLATE("Genre:"),
                                   genre.String(),
                                   new BMessage(kMsgAlbumCommitted));
  fGenreControl->SetModificationMessage(new BMessage(kMsgAlbumEdited));

  fAllCheck = new BCheckBox("all", B_TRANSLATE("All tracks"),
                            new BMessage(kMsgToggleAll));
  fAllCheck->SetValue(B_CONTROL_ON);
  // A checkbox's maximum width is its text width, which would cap the
  // whole window's width (see the other views above).
  fAllCheck->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

  fFormatMenu = new BPopUpMenu("format");
  const struct {
    RipFormat format;
    const char *label;
  } formats[] = {
      {RipFormat::MP3_320, B_TRANSLATE("MP3 (320 kbps)")},
      {RipFormat::FLAC, B_TRANSLATE("FLAC (lossless)")},
      {RipFormat::WAV, B_TRANSLATE("WAV (uncompressed)")},
  };
  for (const auto &f : formats) {
    BMessage *m = new BMessage(kMsgFormatChanged);
    m->AddInt32("format", (int32)f.format);
    fFormatMenu->AddItem(new BMenuItem(f.label, m));
  }
  fFormatField = new BMenuField("format", B_TRANSLATE("Format:"), fFormatMenu);

  fFolderControl = new BTextControl("folder", B_TRANSLATE("Save to:"), "",
                                    new BMessage(kMsgFolderEdited));
  fFolderControl->SetModificationMessage(new BMessage(kMsgFolderEdited));
  fChooseButton = new BButton("choose", B_TRANSLATE("Choose" B_UTF8_ELLIPSIS),
                              new BMessage(kMsgChooseFolder));

  fPatternControl = new BTextControl("pattern", B_TRANSLATE("File names:"), "",
                                     new BMessage(kMsgPatternEdited));
  fPatternControl->SetModificationMessage(new BMessage(kMsgPatternEdited));

  BPopUpMenu *presets = new BPopUpMenu(B_TRANSLATE("Presets"), false, false);
  for (const Preset &p : kPresets) {
    BMessage *m = new BMessage(kMsgPreset);
    m->AddString("pattern", p.pattern);
    presets->AddItem(new BMenuItem(B_TRANSLATE(p.label), m));
  }
  fPresetField = new BMenuField("presets", nullptr, presets);

  BString fields;
  for (int32 i = 0; i < RipNamePattern::kFieldCount; i++) {
    if (i > 0)
      fields << "  ";
    fields << RipNamePattern::kFields[i];
  }
  BStringView *help =
      new BStringView("help", B_TRANSLATE("Use / to make subfolders. Fields:"));
  BStringView *fieldList = new BStringView("fields", fields.String());
  // A string view's maximum width defaults to its text width, which would
  // cap how wide the whole grid -- and the window -- can get.
  help->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
  fieldList->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

  fPreview = new BStringView("preview", "");
  fPreview->SetTruncation(B_TRUNCATE_MIDDLE);
  fPreview->SetExplicitMinSize(BSize(200, B_SIZE_UNSET));
  fPreview->SetExplicitPreferredSize(BSize(400, B_SIZE_UNSET));
  fPreview->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

  fOffsetControl = new BTextControl("offset", B_TRANSLATE("Read offset:"), "",
                                    new BMessage(kMsgOffsetEdited));
  fOffsetControl->SetModificationMessage(new BMessage(kMsgOffsetEdited));
  fDriveInfo = new BStringView("drive", "");
  fDriveInfo->SetTruncation(B_TRUNCATE_END);
  fDriveInfo->SetExplicitMinSize(BSize(100, B_SIZE_UNSET));
  fDriveInfo->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
  fGiveUpControl = new BTextControl("give_up", B_TRANSLATE("Give up at:"), "",
                                    new BMessage(kMsgGiveUpEdited));
  fGiveUpControl->SetModificationMessage(new BMessage(kMsgGiveUpEdited));
  BStringView *giveUpInfo = new BStringView(
      "give_up_info",
      B_TRANSLATE("% of a track unreadable (0 to 10; anything else: never, "
                  "only Skip track)"));
  giveUpInfo->SetTruncation(B_TRUNCATE_END);
  giveUpInfo->SetExplicitMinSize(BSize(100, B_SIZE_UNSET));
  giveUpInfo->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
  fVerifyCheck = new BCheckBox(
      "verify", B_TRANSLATE("Check rips with AccurateRip (needs internet)"),
      new BMessage(kMsgVerifyChanged));
  fVerifyCheck->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
  fLibraryCheck = new BCheckBox("library", B_TRANSLATE("Add tracks to library"),
                                nullptr);
  fLibraryCheck->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
  fLibraryCheck->SetToolTip(B_TRANSLATE(
      "If the save folder isn't one of the library's folders yet, it is "
      "added to them."));

  fProgress = new BStatusBar("progress");
  fProgress->SetMaxValue(kProgressMax);

  fShowButton = new BButton("show", B_TRANSLATE("Show folder"),
                            new BMessage(kMsgShowFolder));
  fCloseButton =
      new BButton("close", B_TRANSLATE("Close"), new BMessage(kMsgClose));
  fRipButton = new BButton("rip", B_TRANSLATE("Rip"), new BMessage(kMsgRip));
  // Gives up on the track being ripped (a hopelessly damaged one) and goes on
  // with the next; only shown while ripping.
  fSkipButton = new BButton("skip", B_TRANSLATE("Skip track"),
                            new BMessage(kMsgSkipTrack));

  BLayoutItem *offsetText = fOffsetControl->CreateTextViewLayoutItem();
  offsetText->SetExplicitMinSize(BSize(60, B_SIZE_UNSET));
  offsetText->SetExplicitMaxSize(BSize(60, B_SIZE_UNSET));
  BLayoutItem *giveUpText = fGiveUpControl->CreateTextViewLayoutItem();
  giveUpText->SetExplicitMinSize(BSize(60, B_SIZE_UNSET));
  giveUpText->SetExplicitMaxSize(BSize(60, B_SIZE_UNSET));

  // clang-format off
  BLayoutBuilder::Group<>(this, B_VERTICAL)
    .SetInsets(B_USE_WINDOW_INSETS)
    .Add(fDiscInfo)
    .AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
      .AddTextControl(fAlbumArtistControl, 0, 0)
      .AddTextControl(fAlbumControl, 0, 1)
      .AddTextControl(fYearControl, 2, 0)
      .AddTextControl(fGenreControl, 2, 1)
      .SetColumnWeight(1, 3.0f)
    .End()
    .Add(fAllCheck)
    .Add(fTrackList, 10)
    .AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
      .Add(fFormatField->CreateLabelLayoutItem(), 0, 0)
      .Add(fFormatField->CreateMenuBarLayoutItem(), 1, 0)
      .AddTextControl(fFolderControl, 0, 1)
      .Add(fChooseButton, 2, 1)
      .AddTextControl(fPatternControl, 0, 2)
      .Add(fPresetField, 2, 2)
      .Add(help, 1, 3, 2)
      .Add(fieldList, 1, 4, 2)
      .Add(fPreview, 1, 5, 2)
      .Add(fOffsetControl->CreateLabelLayoutItem(), 0, 6)
      .AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING, 1, 6, 2)
        .Add(offsetText)
        .Add(fDriveInfo)
      .End()
      .Add(fGiveUpControl->CreateLabelLayoutItem(), 0, 7)
      .AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING, 1, 7, 2)
        .Add(giveUpText)
        .Add(giveUpInfo)
      .End()
      .Add(fVerifyCheck, 1, 8, 2)
      .Add(fLibraryCheck, 1, 9, 2)
    .End()
    .Add(fProgress)
    .AddGroup(B_HORIZONTAL)
      .Add(fShowButton)
      .AddGlue()
      .Add(fCloseButton)
      .Add(fSkipButton)
      .Add(fRipButton)
    .End();
  // clang-format on

  fRipButton->MakeDefault(true);
}

void CDRipWindow::_ApplySettings() {
  BMenuItem *item = fFormatMenu->ItemAt((int32)fSettings.format);
  if (item)
    item->SetMarked(true);
  fFolderControl->SetText(fSettings.directory.String());
  fPatternControl->SetText(fSettings.pattern.String());
  fVerifyCheck->SetValue(fSettings.verify ? B_CONTROL_ON : B_CONTROL_OFF);
  fLibraryCheck->SetValue(fSettings.addToLibrary ? B_CONTROL_ON
                                                 : B_CONTROL_OFF);
  BString giveUp;
  if (fSettings.giveUpPercent >= 0)
    giveUp << fSettings.giveUpPercent;
  fGiveUpControl->SetText(giveUp.String());

  int32 offset = 0;
  BString text;
  if (fSettings.DriveOffset(fDriveName, offset))
    text.SetToFormat("%+ld", (long)offset);
  fOffsetControl->SetText(text.String());
  _UpdateDriveInfo();
}

bool CDRipWindow::_ParseOffset(int32 &offset, bool &known) const {
  BString text(fOffsetControl->Text());
  text.Trim();
  known = !text.IsEmpty();
  offset = 0;
  if (!known)
    return true;
  char *end = nullptr;
  long value = strtol(text.String(), &end, 10);
  if (end == text.String() || *end != '\0' ||
      labs(value) >= AccurateRip::kSkipFrames)
    return false;
  offset = (int32)value;
  return true;
}

int32 CDRipWindow::_ParseGiveUp() const {
  // Anything but a whole number from 0 to 10 (empty, 11, "never") turns
  // giving up off; only Skip track ends a track early then.
  BString text(fGiveUpControl->Text());
  text.Trim();
  char *end = nullptr;
  long value = strtol(text.String(), &end, 10);
  if (text.IsEmpty() || end == text.String() || *end != '\0' || value < 0 ||
      value > 10)
    return -1;
  return (int32)value;
}

void CDRipWindow::_ShowLiveUnreadable(int32 track, int32 sectors) {
  if (sectors <= 0)
    return;
  BRow *row = fTrackList->RowAt(track);
  if (row == nullptr)
    return;
  auto *field = static_cast<BStringField *>(row->GetField(kColErrors));
  if (field == nullptr)
    return;
  // A sector is 1/75 s of audio, filled with silence.
  BString text;
  if (sectors == 1)
    text.SetToFormat(B_TRANSLATE("1 unreadable sector (%.2f s)"),
                     sectors / 75.0);
  else
    text.SetToFormat(B_TRANSLATE("%ld unreadable sectors (%.1f s)"),
                     (long)sectors, sectors / 75.0);
  if (text != field->String()) {
    field->SetString(text.String());
    fTrackList->UpdateRow(row);
  }
  // Red at once: an unreadable sector grades the track Severe when it is
  // done (CDRipper::GradeErrors()). The result at the end still decides; an
  // AccurateRip match turns it green.
  _SetTrackResult(track, kResultBad);
}

void CDRipWindow::_ShowTrackElapsed(int32 index) {
  if (index < 0 || index >= (int32)fTrackStarted.size() ||
      fTrackStarted[index] == 0)
    return;
  BRow *row = fTrackList->RowAt(index);
  if (row == nullptr)
    return;
  auto *field = static_cast<BStringField *>(row->GetField(kColElapsed));
  if (field == nullptr)
    return;
  const BString text = FormatDuration(
      (int32)((system_time() - fTrackStarted[index]) / 1000000));
  if (text != field->String()) {
    field->SetString(text.String());
    fTrackList->UpdateRow(row);
  }
}

void CDRipWindow::_ShowTotalElapsed() {
  if (fRipStarted == 0)
    return;
  BString text(fPercentText);
  if (!text.IsEmpty())
    text << "  ";
  text << FormatDuration((int32)((system_time() - fRipStarted) / 1000000));
  fProgress->SetTrailingText(text.String());
}

void CDRipWindow::_UpdateDriveInfo() {
  BString drive = fDriveName.IsEmpty() ? BString(B_TRANSLATE("this drive"))
                                       : fDriveName;
  BString text;
  int32 offset;
  bool known;
  BString current(fOffsetControl->Text());
  current.Trim();
  if (!fDetectedOffset.IsEmpty() && current == fDetectedOffset) {
    if (fDetectedFromList)
      text.SetToFormat(B_TRANSLATE("samples, from AccurateRip's drive list "
                                   "for %s"), drive.String());
    else
      text.SetToFormat(B_TRANSLATE("samples, detected from AccurateRip for %s"),
                       drive.String());
  } else if (_ParseOffset(offset, known) && !known) {
    text.SetToFormat(B_TRANSLATE("unknown for %s: found from AccurateRip "
                                 "when you rip"), drive.String());
  } else {
    text.SetToFormat(B_TRANSLATE("samples, for %s"), drive.String());
  }
  fDriveInfo->SetText(text.String());
}

bool CDRipWindow::_IsTrackChecked(int32 index) const {
  BRow *row = fTrackList->RowAt(index);
  CheckField *field =
      row ? dynamic_cast<CheckField *>(row->GetField(kColCheck)) : nullptr;
  return field != nullptr && field->checked;
}

int32 CDRipWindow::_CountCheckedTracks() const {
  int32 count = 0;
  for (int32 i = 0; i < fTrackList->CountRows(); i++)
    count += _IsTrackChecked(i) ? 1 : 0;
  return count;
}

void CDRipWindow::_CheckAllTracks(bool checked) {
  for (int32 i = 0; i < fTrackList->CountRows(); i++) {
    BRow *row = fTrackList->RowAt(i);
    if (auto *field = dynamic_cast<CheckField *>(row->GetField(kColCheck)))
      field->checked = checked;
    fTrackList->UpdateRow(row);
  }
  _UpdateAllCheck();
}

void CDRipWindow::_UpdateAllCheck() {
  const int32 checked = _CountCheckedTracks();
  if (checked == 0)
    fAllCheck->SetValue(B_CONTROL_OFF);
  else if (checked == fTrackList->CountRows())
    fAllCheck->SetValue(B_CONTROL_ON);
  else
    fAllCheck->SetValue(B_CONTROL_PARTIALLY_ON);
  _UpdatePreview();
}

void CDRipWindow::_SetTrackResult(int32 index, int32 result) {
  BRow *row = fTrackList->RowAt(index);
  if (row == nullptr)
    return;
  for (int32 c = 0; c < fTrackList->CountColumns(); c++) {
    if (auto *field = dynamic_cast<TrackField *>(row->GetField(c)))
      field->result = (TrackResult)result;
  }
  fTrackList->UpdateRow(row);
}

void CDRipWindow::_SetTrackStatus(int32 index, const char *text) {
  BRow *row = fTrackList->RowAt(index);
  if (row == nullptr)
    return;
  BStringField *field = static_cast<BStringField *>(row->GetField(kColStatus));
  if (field != nullptr)
    field->SetString(text);
  fTrackList->UpdateRow(row);
}

RipFormat CDRipWindow::_SelectedFormat() const {
  int32 index = fFormatMenu->IndexOf(fFormatMenu->FindMarked());
  if (index < (int32)RipFormat::MP3_320 || index > (int32)RipFormat::WAV)
    return RipFormat::FLAC;
  return (RipFormat)index;
}

void CDRipWindow::_ReadControls() {
  fSettings.format = _SelectedFormat();
  fSettings.directory = fFolderControl->Text();
  fSettings.directory.Trim();
  while (fSettings.directory.Length() > 1 && fSettings.directory.EndsWith("/"))
    fSettings.directory.Truncate(fSettings.directory.Length() - 1);
  fSettings.pattern = fPatternControl->Text();
  fSettings.pattern.Trim();
  fSettings.verify = fVerifyCheck->Value() == B_CONTROL_ON;
  fSettings.addToLibrary = fLibraryCheck->Value() == B_CONTROL_ON;
  fSettings.giveUpPercent = _ParseGiveUp();
}

void CDRipWindow::_UpdatePreview() {
  _ReadControls();

  BString text;
  bool ready = false;
  int32 offset;
  bool offsetKnown;
  if (fTracks.empty()) {
    text = B_TRANSLATE("No audio tracks were found on this disc.");
  } else if (!fSettings.directory.StartsWith("/")) {
    text = B_TRANSLATE("Choose the folder to save the tracks in.");
  } else if (fSettings.pattern.IsEmpty()) {
    text = B_TRANSLATE("Enter a pattern for the file names.");
  } else if (!_ParseOffset(offset, offsetKnown)) {
    text = B_TRANSLATE("The read offset must be a whole number of samples "
                       "below 2940, such as +6. Leave it empty to detect it.");
  } else {
    BString unknown;
    RipNamePattern::Expand(fSettings.pattern, fTracks.front(), &unknown);
    CDRipJob job;
    job.format = fSettings.format;
    job.directory = fSettings.directory;
    job.pattern = fSettings.pattern;
    BString example = CDRipper::DestinationFor(job, fTracks.front());
    if (!unknown.IsEmpty()) {
      text.SetToFormat(B_TRANSLATE("%s is not a field and is kept as text: %s"),
                       unknown.String(), example.String());
    } else {
      text.SetToFormat(B_TRANSLATE("Example: %s"), example.String());
    }
    ready = true;
  }

  if (ready && _CountCheckedTracks() == 0) {
    text = B_TRANSLATE("No tracks are selected.");
    ready = false;
  }

  fPreview->SetText(text.String());
  if (fRipper == nullptr)
    fRipButton->SetEnabled(ready);
}

void CDRipWindow::_ChooseFolder() {
  if (fFolderPanel == nullptr) {
    BMessenger self(this);
    fFolderPanel = new BFilePanel(B_OPEN_PANEL, &self, nullptr, B_DIRECTORY_NODE,
                                  false, new BMessage(kMsgFolderChosen), nullptr,
                                  true, true);
    fFolderPanel->Window()->SetTitle(B_TRANSLATE("Beton: Choose rip folder"));
    fFolderPanel->SetButtonLabel(B_DEFAULT_BUTTON, B_TRANSLATE("Select"));
  }

  // Open where the current folder is, or its closest existing parent.
  BPath start(fFolderControl->Text());
  while (start.InitCheck() == B_OK && !BEntry(start.Path()).Exists()) {
    if (start.GetParent(&start) != B_OK)
      break;
  }
  if (start.InitCheck() == B_OK) {
    entry_ref ref;
    if (get_ref_for_path(start.Path(), &ref) == B_OK)
      fFolderPanel->SetPanelDirectory(&ref);
  }
  fFolderPanel->Show();
}

void CDRipWindow::_SetRipping(bool ripping) {
  fFormatField->SetEnabled(!ripping);
  fFolderControl->SetEnabled(!ripping);
  fChooseButton->SetEnabled(!ripping);
  fPatternControl->SetEnabled(!ripping);
  fPresetField->SetEnabled(!ripping);
  fOffsetControl->SetEnabled(!ripping);
  fVerifyCheck->SetEnabled(!ripping);
  fLibraryCheck->SetEnabled(!ripping);
  fGiveUpControl->SetEnabled(!ripping);
  fAllCheck->SetEnabled(!ripping);
  fAlbumArtistControl->SetEnabled(!ripping);
  fAlbumControl->SetEnabled(!ripping);
  fYearControl->SetEnabled(!ripping);
  fGenreControl->SetEnabled(!ripping);
  static_cast<CheckColumn *>(fCheckColumn)->SetLocked(ripping);
  fTrackList->Invalidate();
  fShowButton->SetEnabled(!fOutputFolder.IsEmpty());
  fRipButton->SetLabel(ripping ? B_TRANSLATE("Stop") : B_TRANSLATE("Rip"));
  fRipButton->SetEnabled(true);
  fSkipButton->SetEnabled(ripping);
  if (ripping && fSkipButton->IsHidden())
    fSkipButton->Show();
  else if (!ripping && !fSkipButton->IsHidden())
    fSkipButton->Hide();
}

void CDRipWindow::_StartRip() {
  // The rip takes the names as they are now: an edit in progress counts,
  // and they are on the disc too before it starts.
  if (fEditor != nullptr)
    _FinishEdit(true, 0);
  _ApplyAlbumFields(true);

  _ReadControls();
  int32 offset = 0;
  bool offsetKnown = false;
  if (fTracks.empty() || !fSettings.directory.StartsWith("/") ||
      fSettings.pattern.IsEmpty() || !_ParseOffset(offset, offsetKnown))
    return;

  // An emptied offset field means "detect it again".
  if (offsetKnown)
    fSettings.SetDriveOffset(fDriveName, offset);
  else
    fSettings.ForgetDriveOffset(fDriveName);
  fSettings.Save();

  CDRipJob job;
  job.discPath = fDiscPath;
  job.tracks = fTracks;
  job.format = fSettings.format;
  job.directory = fSettings.directory;
  job.pattern = fSettings.pattern;
  job.verify = fSettings.verify;
  job.offsetKnown = offsetKnown;
  job.offset = offset;
  job.driveName = fDriveName;
  job.giveUpPercent = fSettings.giveUpPercent;
  for (int32 i = 0; i < (int32)fTracks.size(); i++)
    job.selected.push_back(_IsTrackChecked(i));

  for (int32 i = 0; i < (int32)fTracks.size(); i++) {
    if (BRow *row = fTrackList->RowAt(i)) {
      for (int32 column : {kColGain, kColErrors, kColElapsed}) {
        if (auto *field = static_cast<BStringField *>(row->GetField(column)))
          field->SetString("");
      }
    }
    _SetTrackStatus(i, _IsTrackChecked(i) ? "" : B_TRANSLATE("Not selected"));
    _SetTrackResult(i, kResultNone);
  }
  fCurrentTrack = -1;
  fCurrentPercent = -1;

  fFailures.clear();
  fOutputFolder.Truncate(0);
  fRippedPaths.assign(fTracks.size(), BString());

  fRipper = new CDRipper(job, BMessenger(this));
  status_t status = fRipper->Start();
  if (status != B_OK) {
    delete fRipper;
    fRipper = nullptr;
    BString text;
    text.SetToFormat(B_TRANSLATE("Could not start ripping: %s"), strerror(status));
    (new BAlert(B_TRANSLATE("Rip audio CD"), text.String(), B_TRANSLATE("OK"),
                nullptr, nullptr, B_WIDTH_AS_USUAL, B_STOP_ALERT))
        ->Go(nullptr);
    return;
  }

  fProgress->Reset();
  fProgress->SetMaxValue(kProgressMax);
  fProgress->SetTo(0, B_TRANSLATE("Starting" B_UTF8_ELLIPSIS), "");
  _SetRipping(true);

  fTrackStarted.assign(fTracks.size(), 0);
  fTimingTrack = -1;
  fPercentText = "";
  fRipStarted = system_time();
  delete fTicker;
  BMessage tick(kMsgTick);
  fTicker = new BMessageRunner(BMessenger(this), &tick, 1000000);
}

void CDRipWindow::_HandleProgress(BMessage *msg) {
  int32 track = 0, tracks = 0;
  float fraction = 0, trackFraction = 0;
  BString title;
  msg->FindInt32("track", &track);
  msg->FindInt32("tracks", &tracks);
  msg->FindFloat("fraction", &fraction);
  msg->FindFloat("track_fraction", &trackFraction);
  msg->FindString("title", &title);

  const int32 trackPercent = (int32)(trackFraction * 100.0f);
  if (track != fCurrentTrack || trackPercent != fCurrentPercent) {
    fCurrentTrack = track;
    fCurrentPercent = trackPercent;
    BString status;
    status.SetToFormat(B_TRANSLATE("Ripping %ld%%"), (long)trackPercent);
    _SetTrackStatus(track, status.String());
    if (BRow *row = fTrackList->RowAt(track))
      fTrackList->ScrollTo(row);
  }

  int32 unreadable = 0;
  if (msg->FindInt32("unreadable", &unreadable) == B_OK)
    _ShowLiveUnreadable(track, unreadable);

  BString text;
  text.SetToFormat(B_TRANSLATE("Track %ld of %ld: %s"), (long)track + 1,
                   (long)tracks, title.String());
  // A track's clock starts with its first progress message.
  if (track != fTimingTrack && track >= 0 &&
      track < (int32)fTrackStarted.size()) {
    fTimingTrack = track;
    if (fTrackStarted[track] == 0)
      fTrackStarted[track] = system_time();
  }
  _ShowTrackElapsed(track);

  fPercentText.SetToFormat("%d%%", (int)(fraction * 100.0f));
  fProgress->SetTo(fraction * kProgressMax, text.String(),
                   fPercentText.String());
  _ShowTotalElapsed();
}

void CDRipWindow::_HandleTrackDone(BMessage *msg) {
  int32 track = 0;
  int32 status = B_OK;
  BString path, title, error;
  msg->FindInt32("track", &track);
  msg->FindInt32("status", &status);
  msg->FindString("path", &path);
  msg->FindString("title", &title);
  msg->FindString("error", &error);
  int32 verdict = kRipNotChecked, confidence = -1;
  msg->FindInt32("ar", &verdict);
  msg->FindInt32("ar_confidence", &confidence);

  BString text;
  if (status == B_OK) {
    switch (verdict) {
    case kRipAccurate:
      if (confidence == 1)
        text = B_TRANSLATE("Accurate (1 matching rip)");
      else
        text.SetToFormat(B_TRANSLATE("Accurate (%ld matching rips)"),
                         (long)confidence);
      break;
    case kRipNoMatch:
      text = B_TRANSLATE("Ripped, no AccurateRip match");
      break;
    case kRipNotInDatabase:
      text = B_TRANSLATE("Ripped (not in AccurateRip)");
      break;
    default:
      text = B_TRANSLATE("Ripped");
    }
    float gain;
    if (msg->FindFloat("rg_gain", &gain) == B_OK) {
      BString gainText;
      gainText.SetToFormat("%+.1f dB", gain);
      if (BRow *row = fTrackList->RowAt(track)) {
        if (auto *field = static_cast<BStringField *>(row->GetField(kColGain)))
          field->SetString(gainText.String());
      }
    }
  } else if (status == B_FILE_EXISTS) {
    text = B_TRANSLATE("Already there, not ripped again");
  } else if (status == B_INTERRUPTED) {
    // Skip track, or the ripper gave up on it; no file written either way.
    const int32 percent = msg->GetInt32("give_up_percent", -1);
    if (msg->GetBool("gave_up", false) && percent == 0)
      text = B_TRANSLATE("Gave up at the first unreadable sector, not ripped");
    else if (msg->GetBool("gave_up", false))
      text.SetToFormat(B_TRANSLATE("Gave up: over %ld%% unreadable, not "
                                   "ripped"), (long)percent);
    else
      text = B_TRANSLATE("Skipped, not ripped");
  } else {
    text.SetToFormat(B_TRANSLATE("Failed: %s"),
                     error.IsEmpty() ? strerror(status) : error.String());
  }
  bool checked = false;
  msg->FindBool("errors_checked", &checked);
  int32 grade = CDRipper::kErrorsNone;
  msg->FindInt32("err_grade", &grade);

  // Errors left: yellow, orange or red by how audible they are likely to be;
  // red also for a failed rip. Green: nothing left to fix, vivid when
  // AccurateRip confirms it -- which wins over flagged bytes, as a match
  // proves the audio exact. With neither check available, Beton can't vouch
  // for the track and leaves it uncolored.
  TrackResult result = kResultNone;
  if (status != B_OK && status != B_FILE_EXISTS)
    result = kResultBad;
  else if (status == B_OK && verdict == kRipAccurate)
    result = kResultVerified;
  else if (status == B_OK && grade == CDRipper::kErrorsSevere)
    result = kResultBad;
  else if (status == B_OK && grade == CDRipper::kErrorsNoticeable)
    result = kResultNoticeable;
  else if (status == B_OK && grade == CDRipper::kErrorsMinor)
    result = kResultMinor;
  else if (status == B_OK && checked)
    result = kResultClean;
  _SetTrackResult(track, result);
  if ((status == B_OK || status == B_FILE_EXISTS) && !path.IsEmpty() &&
      track >= 0 && track < (int32)fRippedPaths.size())
    fRippedPaths[track] = path;
  // Each track joins the library as soon as it is done (its file is complete
  // by now). The end of the rip reads them all again for the album loudness.
  if (status == B_OK && !path.IsEmpty() && fSettings.addToLibrary &&
      fLibraryTarget.IsValid()) {
    BMessage import(MSG_CD_RIP_IMPORT);
    import.AddBool("add", true);
    import.AddString("path", path);
    fLibraryTarget.SendMessage(&import);
  }
  if (BRow *row = fTrackList->RowAt(track)) {
    if (auto *field = static_cast<BStringField *>(row->GetField(kColErrors))) {
      BString errors;
      if (status == B_OK && checked) {
        int32 fixed = 0, unreadable = 0, glitches = 0, longest = 0;
        msg->FindInt32("err_fixed", &fixed);
        msg->FindInt32("err_unreadable", &unreadable);
        msg->FindInt32("err_glitches", &glitches);
        msg->FindInt32("err_longest_frames", &longest);
        if (glitches == 0) {
          // "Fixed" sectors (1/75 s) were flagged but read clean again.
          if (fixed > 0)
            errors.SetToFormat(B_TRANSLATE("None (%ld fixed)"), (long)fixed);
          else
            errors = B_TRANSLATE("None");
        } else {
          switch (grade) {
          case CDRipper::kErrorsSevere:
            errors = B_TRANSLATE("Severe");
            break;
          case CDRipper::kErrorsNoticeable:
            errors = B_TRANSLATE("Noticeable");
            break;
          default:
            errors = B_TRANSLATE("Minor");
            break;
          }
          BString part;
          if (glitches == 1)
            part = B_TRANSLATE("1 glitch");
          else
            part.SetToFormat(B_TRANSLATE("%ld glitches"), (long)glitches);
          errors << ": " << part;
          (void)longest;
          // The worst glitch: how much it stands out from the music around
          // it (0 dB or less: masked), and where, to listen to it.
          float db = 0;
          int64 atMs = -1;
          if (msg->FindFloat("err_loudest_db", &db) == B_OK &&
              msg->FindInt64("err_loudest_at_ms", &atMs) == B_OK) {
            const int64 secs = atMs / 1000;
            BString loudest;
            loudest.SetToFormat(B_TRANSLATE("worst %+ld dB at %ld:%02ld"),
                                (long)floor(db + 0.5), (long)(secs / 60),
                                (long)(secs % 60));
            errors << ", " << loudest;
          }
          if (unreadable > 0) {
            BString gaps;
            gaps.SetToFormat(B_TRANSLATE("%ld ms silent"),
                             (long)(unreadable * 1000 / 75));
            errors << ", " << gaps;
          }
          if (verdict == kRipAccurate)
            errors << " " << B_TRANSLATE("(audio verified exact)");
        }
      } else if (status == B_OK) {
        errors = B_TRANSLATE("Not checked");
      } else if (status == B_INTERRUPTED) {
        // Skipped or given up: keep what was found, which is why.
        const int32 unreadable = msg->GetInt32("err_unreadable", 0);
        if (unreadable == 1)
          errors.SetToFormat(B_TRANSLATE("1 unreadable sector (%.2f s)"),
                             unreadable / 75.0);
        else if (unreadable > 1)
          errors.SetToFormat(B_TRANSLATE("%ld unreadable sectors (%.1f s)"),
                             (long)unreadable, unreadable / 75.0);
      }
      field->SetString(errors.String());
    }
  }

  _SetTrackStatus(track, text.String());
  fCurrentTrack = -1;
  // The track's time stops here and stays in Elapsed.
  _ShowTrackElapsed(track);
  if (track == fTimingTrack)
    fTimingTrack = -1;

  if ((status == B_OK || status == B_FILE_EXISTS) && fOutputFolder.IsEmpty()) {
    BPath parent;
    if (BPath(path.String()).GetParent(&parent) == B_OK)
      fOutputFolder = parent.Path();
  }

  if (status != B_OK && status != B_FILE_EXISTS && status != B_INTERRUPTED) {
    BString line;
    line.SetToFormat(B_TRANSLATE("Track %ld (%s): %s"), (long)track + 1,
                     title.String(),
                     error.IsEmpty() ? strerror(status) : error.String());
    fFailures.push_back(line);
  }
}

void CDRipWindow::_HandleFinished(BMessage *msg) {
  if (fRipper) {
    fRipper->Wait();
    delete fRipper;
    fRipper = nullptr;
  }

  int32 ripped = 0, skipped = 0, failed = 0, accurate = 0, noMatch = 0;
  bool cancelled = false;
  BString arNote, error, errorsNote;
  int32 tracksWithErrors = 0;
  msg->FindInt32("ripped", &ripped);
  msg->FindInt32("skipped", &skipped);
  msg->FindInt32("failed", &failed);
  msg->FindInt32("accurate", &accurate);
  msg->FindInt32("no_match", &noMatch);
  msg->FindBool("cancelled", &cancelled);

  // An offset from AccurateRip's drive list is kept for the drive at once.
  // A detected one only once a track confirms it on two different discs: a
  // disc from an unusual pressing matches at the wrong offset on every track
  // (Dead or Alive's "Rip It Up" found +83 for a Slimtype eNAU108 the drive
  // list has at +6, 2026-11-12).
  if (msg->GetBool("offset_detected", false)) {
    int32 offset = 0;
    msg->FindInt32("offset", &offset);
    BString disc;
    msg->FindString("disc_key", &disc);
    int32 earlier = 0;
    BString earlierDisc;
    if (msg->GetBool("offset_from_list", false)) {
      fSettings.SetDriveOffset(fDriveName, offset);
      fSettings.ForgetOffsetCandidate(fDriveName);
      fSettings.Save();
    } else if (accurate > 0 &&
               fSettings.OffsetCandidate(fDriveName, earlier, earlierDisc) &&
               earlier == offset && earlierDisc != disc) {
      fSettings.SetDriveOffset(fDriveName, offset);
      fSettings.ForgetOffsetCandidate(fDriveName);
      fSettings.Save();
    } else if (accurate > 0) {
      // First disc (or the same disc again): remember it, and detect again
      // next time instead of trusting it yet.
      fSettings.SetOffsetCandidate(fDriveName, offset, disc);
      fSettings.Save();
      fOffsetControl->SetText("");
      fDetectedOffset = "";
      fDetectedFromList = false;
      BString text;
      text.SetToFormat(B_TRANSLATE("unknown for %s: %+ld worked for this disc, "
                                   "and is kept once another disc agrees"),
                       fDriveName.IsEmpty() ? B_TRANSLATE("this drive")
                                            : fDriveName.String(),
                       (long)offset);
      fDriveInfo->SetText(text.String());
    } else {
      // Nothing confirmed it (every track failed, or the rip stopped first):
      // find it again next time rather than keep a guess.
      fOffsetControl->SetText("");
      fDetectedOffset = "";
      _UpdateDriveInfo();
    }
  }
  msg->FindString("ar_note", &arNote);
  msg->FindString("error", &error);
  msg->FindString("errors_note", &errorsNote);
  msg->FindInt32("tracks_with_errors", &tracksWithErrors);

  if (cancelled && fCurrentTrack >= 0)
    _SetTrackStatus(fCurrentTrack, B_TRANSLATE("Stopped"));
  fCurrentTrack = -1;

  BString summary;
  if (ripped == 1)
    summary = B_TRANSLATE("1 track ripped");
  else
    summary.SetToFormat(B_TRANSLATE("%ld tracks ripped"), (long)ripped);
  if (skipped > 0) {
    BString part;
    part.SetToFormat(B_TRANSLATE("%ld already there"), (long)skipped);
    summary << ", " << part;
  }
  if (failed > 0) {
    BString part;
    part.SetToFormat(B_TRANSLATE("%ld failed"), (long)failed);
    summary << ", " << part;
  }
  int32 givenUp = 0;
  if (msg->FindInt32("given_up", &givenUp) == B_OK && givenUp > 0) {
    BString part;
    part.SetToFormat(B_TRANSLATE("%ld skipped"), (long)givenUp);
    summary << ", " << part;
  }
  if (accurate + noMatch > 0) {
    BString part;
    part.SetToFormat(B_TRANSLATE("%ld accurate"), (long)accurate);
    summary << ", " << part;
  }
  if (tracksWithErrors > 0) {
    BString part;
    if (tracksWithErrors == 1)
      part = B_TRANSLATE("1 with audible errors");
    else
      part.SetToFormat(B_TRANSLATE("%ld with audible errors"), (long)tracksWithErrors);
    summary << ", " << part;
  }
  if (cancelled)
    summary << " " << B_TRANSLATE("(stopped)");
  else if (!arNote.IsEmpty() && ripped > 0)
    summary << " " << B_TRANSLATE("- AccurateRip:") << " " << arNote;
  if (!cancelled && ripped > 0 && !errorsNote.IsEmpty())
    summary << " " << B_TRANSLATE("- Read errors:") << " " << errorsNote;

  delete fTicker;
  fTicker = nullptr;
  if (fTimingTrack >= 0) {
    // Stopped mid-track: its time so far stays.
    _ShowTrackElapsed(fTimingTrack);
    fTimingTrack = -1;
  }
  BString total;
  if (fRipStarted > 0)
    total.SetToFormat(B_TRANSLATE("Total %s"),
                      FormatDuration((int32)((system_time() - fRipStarted) /
                                             1000000)).String());
  fRipStarted = 0;
  fProgress->SetTo(cancelled ? fProgress->CurrentValue() : kProgressMax,
                   summary.String(), total.String());
  _SetRipping(false);
  _UpdatePreview();

  // Hand the new files over to be added to the library. Sent even when they
  // aren't to be added: in a sidebar folder they are added as they appear,
  // and must be read again now that they are finished.
  if (ripped > 0 && fLibraryTarget.IsValid()) {
    BMessage import(MSG_CD_RIP_IMPORT);
    import.AddBool("add", fSettings.addToLibrary);
    import.AddString("folder", fSettings.directory);
    BString path;
    for (int32 i = 0; msg->FindString("path", i, &path) == B_OK; i++)
      import.AddString("path", path);
    fLibraryTarget.SendMessage(&import);
  }

  if (!error.IsEmpty()) {
    BString text;
    text.SetToFormat(B_TRANSLATE("The disc could not be read: %s"),
                     error.String());
    (new BAlert(B_TRANSLATE("Rip audio CD"), text.String(), B_TRANSLATE("OK"),
                nullptr, nullptr, B_WIDTH_AS_USUAL, B_STOP_ALERT))
        ->Go(nullptr);
  } else if (tracksWithErrors > 0 && !cancelled) {
    BString text;
    text.SetToFormat(
        B_TRANSLATE("%ld tracks still had read errors after re-reading that "
                    "may be heard as clicks or gaps (orange or red). The "
                    "\"Read errors\" column says how many glitches each has "
                    "and how long the longest is. Cleaning the disc and "
                    "ripping those tracks again (delete their files first) "
                    "may help."),
        (long)tracksWithErrors);
    (new BAlert(B_TRANSLATE("Rip audio CD"), text.String(), B_TRANSLATE("OK"),
                nullptr, nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT))
        ->Go(nullptr);
  } else if (noMatch > 0 && !cancelled) {
    // A partial mismatch points at the tracks; a total one at the disc.
    const char *text =
        accurate > 0
            ? B_TRANSLATE("Some tracks don't match AccurateRip, although the "
                          "rest of the disc does. They probably have read "
                          "errors. Cleaning the disc and ripping those tracks "
                          "again (delete their files first) may help.")
            : B_TRANSLATE("No track matches AccurateRip. Your copy may be a "
                          "pressing nobody has submitted yet, or the drive's "
                          "read offset is wrong. Empty the read offset field "
                          "to detect it again.");
    (new BAlert(B_TRANSLATE("Rip audio CD"), text, B_TRANSLATE("OK"), nullptr,
                nullptr, B_WIDTH_AS_USUAL, B_INFO_ALERT))
        ->Go(nullptr);
  }

  if (!fFailures.empty()) {
    BString text(B_TRANSLATE("Some tracks could not be ripped:"));
    text << "\n\n";
    const size_t shown = std::min<size_t>(fFailures.size(), 10);
    for (size_t i = 0; i < shown; i++)
      text << fFailures[i] << "\n";
    if (fFailures.size() > shown)
      text << B_UTF8_ELLIPSIS << "\n";
    (new BAlert(B_TRANSLATE("Rip audio CD"), text.String(), B_TRANSLATE("OK"),
                nullptr, nullptr, B_WIDTH_FROM_LABEL, B_WARNING_ALERT))
        ->Go(nullptr);
  }
}

void CDRipWindow::_HandleOffset(BMessage *msg) {
  if (msg->GetBool("failed", false)) {
    BString text;
    text.SetToFormat(B_TRANSLATE("could not be found for %s on this disc, so "
                                 "this rip isn't checked against "
                                 "AccurateRip. Enter it if you know it."),
                     fDriveName.IsEmpty() ? B_TRANSLATE("this drive")
                                          : fDriveName.String());
    fDriveInfo->SetText(text.String());
    return;
  }

  int32 offset = 0;
  if (msg->FindInt32("offset", &offset) != B_OK)
    return;

  // Shown now, but saved for the drive only at the end (see _HandleFinished):
  // from the drive list at once, a detected one once two discs agree.
  BString text;
  text.SetToFormat("%+ld", (long)offset);
  fOffsetControl->SetText(text.String());
  fDetectedOffset = text;
  fDetectedFromList = msg->GetBool("from_list", false);
  _UpdateDriveInfo();
}

void CDRipWindow::_ShowOutputFolder() {
  BString folder = fOutputFolder.IsEmpty() ? fSettings.directory : fOutputFolder;
  entry_ref ref;
  if (get_ref_for_path(folder.String(), &ref) == B_OK)
    be_roster->Launch(&ref);
}

bool CDRipWindow::_HasRippedFile(int32 index) const {
  return index >= 0 && index < (int32)fRippedPaths.size() &&
         !fRippedPaths[index].IsEmpty() &&
         BEntry(fRippedPaths[index].String()).Exists();
}

void CDRipWindow::_ShowTrackMenu(int32 index, BPoint where) {
  const bool hasFile = _HasRippedFile(index);
  BPopUpMenu *menu = new BPopUpMenu("track", false, false);
  BMenuItem *play =
      new BMenuItem(B_TRANSLATE("Play"), new BMessage(kMsgPlayTrack));
  play->SetEnabled(hasFile);
  menu->AddItem(play);
  menu->AddSeparatorItem();
  const struct {
    const char *label;
    int32 column;
  } edits[] = {{B_TRANSLATE("Edit title"), kColTitle},
               {B_TRANSLATE("Edit artist"), kColArtist}};
  for (const auto &e : edits) {
    BMessage *edit = new BMessage(kMsgStartEdit);
    edit->AddInt32("index", index);
    edit->AddInt32("column", e.column);
    BMenuItem *item = new BMenuItem(e.label, edit);
    item->SetEnabled(fRipper == nullptr);
    menu->AddItem(item);
  }
  menu->AddSeparatorItem();
  BMessage *trash = new BMessage(kMsgTrashTrack);
  trash->AddInt32("index", index);
  BMenuItem *trashItem =
      new BMenuItem(B_TRANSLATE("Move ripped file to Trash"), trash);
  trashItem->SetEnabled(hasFile);
  menu->AddItem(trashItem);
  menu->SetTargetForItems(this);
  menu->SetAsyncAutoDestruct(true);
  menu->Go(where, true, true, true);
}

void CDRipWindow::_TrashRippedFile(int32 index) {
  if (!_HasRippedFile(index))
    return;
  const BString path = fRippedPaths[index];

  // The Trash of the file's own volume: a move can't cross volumes.
  BEntry entry(path.String());
  node_ref nref;
  BVolume volume;
  BPath trash;
  if (entry.GetNodeRef(&nref) != B_OK || volume.SetTo(nref.device) != B_OK ||
      find_directory(B_TRASH_DIRECTORY, &trash, true, &volume) != B_OK) {
    _SetTrackStatus(index, B_TRANSLATE("Could not find the Trash"));
    return;
  }
  // "Name 2.flac" when the Trash already holds one by that name.
  BString leaf(BPath(path.String()).Leaf());
  BPath target(trash.Path(), leaf.String());
  BString base(leaf), ext;
  const int32 dot = leaf.FindLast('.');
  if (dot > 0) {
    leaf.CopyInto(base, 0, dot);
    leaf.CopyInto(ext, dot, leaf.Length() - dot);
  }
  for (int32 n = 2; BEntry(target.Path()).Exists(); n++) {
    BString name;
    name << base << " " << n << ext;
    target.SetTo(trash.Path(), name.String());
  }

  // The main window moves it, as the library's own Move to Trash does: the
  // track leaves the library and its playlists, and Tracker can restore it.
  BMessage move(MSG_FILE_MOVE);
  move.AddString("from", path);
  move.AddString("to", target.Path());
  if (!fLibraryTarget.IsValid() || fLibraryTarget.SendMessage(&move) != B_OK) {
    _SetTrackStatus(index, B_TRANSLATE("Could not move the file to the Trash"));
    return;
  }

  fRippedPaths[index] = "";
  if (BRow *row = fTrackList->RowAt(index)) {
    if (auto *field = static_cast<BStringField *>(row->GetField(kColGain)))
      field->SetString("");
  }
  _SetTrackResult(index, kResultNone);
  _SetTrackStatus(index, B_TRANSLATE("Moved to Trash"));
}

void CDRipWindow::_PlaySelectedTrack() {
  BRow *row = fTrackList->CurrentSelection();
  const int32 clicked = row != nullptr ? fTrackList->IndexOf(row) : -1;
  if (clicked < 0 || clicked >= (int32)fRippedPaths.size() ||
      fRippedPaths[clicked].IsEmpty() || !fLibraryTarget.IsValid())
    return;

  // Queue every ripped track in disc order, so playback carries on to the
  // next one, as it does in the library.
  BMessage play(MSG_CD_RIP_PLAY);
  int32 index = 0;
  for (int32 i = 0; i < (int32)fRippedPaths.size(); i++) {
    if (fRippedPaths[i].IsEmpty())
      continue;
    if (i == clicked)
      play.AddInt32("index", index);
    const MediaItem &t = fTracks[i];
    play.AddString("path", fRippedPaths[i]);
    play.AddString("title", t.title);
    play.AddString("artist", t.artist);
    play.AddString("album", t.album);
    play.AddInt32("track", t.track);
    play.AddInt32("duration", t.duration);
    index++;
  }
  fLibraryTarget.SendMessage(&play);
}

void CDRipWindow::_StartEdit(int32 index, int32 column) {
  if (fRipper != nullptr || index < 0 || index >= (int32)fTracks.size() ||
      (column != kColTitle && column != kColArtist))
    return;
  if (fEditor != nullptr)
    _FinishEdit(true, 0);

  BRow *row = fTrackList->RowAt(index);
  BColumn *col = fTrackList->ColumnAt(column);
  if (row == nullptr || col == nullptr)
    return;
  fTrackList->DeselectAll();
  fTrackList->AddToSelection(row);
  fTrackList->ScrollTo(row);

  const MediaItem &t = fTracks[index];
  fEditor = new CellEditor(fTrackList->GetFieldRect(row, col),
                           column == kColTitle ? t.title.String()
                                               : t.artist.String(),
                           BMessenger(this));
  fEditIndex = index;
  fEditColumn = column;
  // The window takes Escape before any view does, to close itself: not
  // while a cell is edited, where it cancels the edit.
  fSavedFlags = Flags();
  SetFlags(Flags() & ~B_CLOSE_ON_ESCAPE);
  // In the list's own view, so it sits on the cell and scrolls with it.
  fTrackList->ScrollView()->AddChild(fEditor);
  fEditor->MakeFocus(true);
  fEditor->SelectAll();
}

void CDRipWindow::_FinishEdit(bool commit, int32 move) {
  if (fEditor == nullptr)
    return;
  CellEditor *editor = fEditor;
  fEditor = nullptr;
  BString text(editor->Text());
  text.Trim();
  const int32 index = fEditIndex;
  const int32 column = fEditColumn;
  editor->Finish();
  editor->RemoveSelf();
  delete editor;
  SetFlags(fSavedFlags);

  // A track has a title; an empty artist means the album artist's.
  if (commit && index >= 0 && index < (int32)fTracks.size() &&
      !(column == kColTitle && text.IsEmpty())) {
    MediaItem &t = fTracks[index];
    BString &value = column == kColTitle ? t.title : t.artist;
    if (text != value) {
      value = text;
      if (column == kColTitle)
        fTitleFromFile[index] = false;
      if (BRow *row = fTrackList->RowAt(index)) {
        if (auto *field = static_cast<BStringField *>(row->GetField(column)))
          field->SetString(text.String());
        fTrackList->UpdateRow(row);
      }
      const status_t status = _SaveTrackToDisc(index);
      if (status != B_OK) {
        BString message;
        message.SetToFormat(B_TRANSLATE("Not saved to the disc: %s"),
                            strerror(status));
        _SetTrackStatus(index, message.String());
      }
      _UpdatePreview();
    }
  }

  // Tab: Title, Artist, the next track's Title, ...
  if (move != 0 && index >= 0) {
    const int32 cell = index * 2 + (column == kColTitle ? 0 : 1) + move;
    if (cell >= 0 && cell < (int32)fTracks.size() * 2) {
      _StartEdit(cell / 2, cell % 2 == 0 ? kColTitle : kColArtist);
      return;
    }
  }
  fTrackList->MakeFocus(true);
}

void CDRipWindow::DispatchMessage(BMessage *msg, BHandler *target) {
  // The window gives Enter to the default button (Rip) whatever has the
  // focus; in a cell being edited it saves the edit instead.
  if (fEditor != nullptr && CurrentFocus() == fEditor &&
      (msg->what == B_KEY_DOWN || msg->what == B_KEY_UP) &&
      target == DefaultButton())
    target = fEditor;
  BWindow::DispatchMessage(msg, target);
}

void CDRipWindow::_ApplyAlbumFields(bool save) {
  BString artist(fAlbumArtistControl->Text());
  artist.Trim();
  BString album(fAlbumControl->Text());
  album.Trim();
  const int32 year = atoi(fYearControl->Text());
  BString genre(fGenreControl->Text());
  genre.Trim();
  for (size_t i = 0; i < fTracks.size(); i++) {
    MediaItem &t = fTracks[i];
    // A track by the album artist follows it; a compilation's own track
    // artists stay.
    if (t.artist == fAlbumArtist && t.artist != artist) {
      t.artist = artist;
      if (BRow *row = fTrackList->RowAt((int32)i)) {
        if (auto *field = static_cast<BStringField *>(row->GetField(kColArtist)))
          field->SetString(artist.String());
        fTrackList->UpdateRow(row);
      }
    }
    t.albumArtist = artist;
    t.album = album;
    t.year = year > 0 ? year : 0;
    t.genre = genre;
  }
  fAlbumArtist = artist;
  _UpdatePreview();

  if (save && fAlbumDirty) {
    fAlbumDirty = false;
    _SaveAlbumToDisc();
  }
}

status_t CDRipWindow::_SaveTrackToDisc(int32 index) {
  MediaItem &t = fTracks[index];
  BNode node(t.path.String());
  status_t status = node.InitCheck();
  if (status != B_OK)
    return status;

  // The attributes cddb_lookup writes (cdda keeps them with the disc).
  if (!fTitleFromFile[index])
    node.WriteAttrString("Media:Title", &t.title);
  if (!t.artist.IsEmpty())
    node.WriteAttrString("Audio:Artist", &t.artist);
  else
    node.RemoveAttr("Audio:Artist");
  if (!t.album.IsEmpty())
    node.WriteAttrString("Audio:Album", &t.album);
  else
    node.RemoveAttr("Audio:Album");
  if (t.year > 0)
    node.WriteAttr("Media:Year", B_INT32_TYPE, 0, &t.year, sizeof(t.year));
  else
    node.RemoveAttr("Media:Year");
  // Stock cdda wrote CD-Text's genre as Audio:Genre, which CddaTracks
  // falls back to: drop it so it can't come back.
  node.RemoveAttr("Audio:Genre");
  if (!t.genre.IsEmpty())
    node.WriteAttrString("Media:Genre", &t.genre);
  else
    node.RemoveAttr("Media:Genre");

  // A track nobody named keeps cdda's "Track 01.wav".
  if (fTitleFromFile[index])
    return B_OK;

  // Named as cddb_lookup names it: "01 Title.wav", or "01 Artist -
  // Title.wav" for a track with an artist of its own.
  BPath path(t.path.String());
  if (path.InitCheck() != B_OK)
    return path.InitCheck();
  BString leaf(path.Leaf());
  BString extension;
  const int32 dot = leaf.FindLast('.');
  if (dot > 0)
    leaf.CopyInto(extension, dot, leaf.Length() - dot);
  BString name;
  name.SetToFormat("%02ld ", (long)t.track);
  if (!t.artist.IsEmpty() && t.artist != t.albumArtist)
    name << t.artist << " - ";
  name << t.title;
  name.ReplaceSet("/", " ");
  // Room for the extension in a file name (B_FILE_NAME_LENGTH bytes).
  const int32 room = B_FILE_NAME_LENGTH - 1 - extension.Length();
  if (name.Length() > room)
    name.TruncateChars(room);
  while (name.Length() > room)
    name.TruncateChars(name.CountChars() - 1);
  name << extension;
  if (name == leaf)
    return B_OK;

  BEntry entry(t.path.String());
  status = entry.Rename(name.String());
  if (status != B_OK)
    return status;
  BPath renamed;
  if (entry.GetPath(&renamed) == B_OK)
    t.path = renamed.Path();
  return B_OK;
}

void CDRipWindow::_SaveAlbumToDisc() {
  if (fTracks.empty())
    return;
  const BString album = fTracks.front().album;

  BNode root(fDiscPath.String());
  if (root.InitCheck() == B_OK) {
    // CddaTracks reads the album artist from here.
    if (!fAlbumArtist.IsEmpty())
      root.WriteAttrString("Audio:Artist", &fAlbumArtist);
    else
      root.RemoveAttr("Audio:Artist");
    // Named now: the matches cddb_lookup could only offer are done with.
    root.RemoveAttr("CDDB:candidates");
  }

  for (int32 i = 0; i < (int32)fTracks.size(); i++) {
    const status_t status = _SaveTrackToDisc(i);
    if (status != B_OK) {
      BString message;
      message.SetToFormat(B_TRANSLATE("Not saved to the disc: %s"),
                          strerror(status));
      _SetTrackStatus(i, message.String());
    }
  }

  // The volume last: renaming it renames its mount point, which is in
  // every track's path.
  BString name(fAlbumArtist);
  if (!album.IsEmpty()) {
    if (!name.IsEmpty())
      name << " - ";
    name << album;
  }
  name.ReplaceSet("/", " ");
  while (name.Length() > B_FILE_NAME_LENGTH - 1)
    name.TruncateChars(name.CountChars() - 1);
  if (name.IsEmpty())
    return;
  BVolume volume(dev_for_path(fDiscPath.String()));
  if (volume.InitCheck() != B_OK || volume.SetName(name.String()) != B_OK)
    return;
  fDiscLabel = name;
  _UpdateDiscInfo();

  BDirectory rootDir;
  BEntry rootEntry;
  BPath rootPath;
  if (volume.GetRootDirectory(&rootDir) != B_OK ||
      rootDir.GetEntry(&rootEntry) != B_OK ||
      rootEntry.GetPath(&rootPath) != B_OK || fDiscPath == rootPath.Path())
    return;
  const BString old = fDiscPath;
  BString oldPrefix(old);
  oldPrefix << '/';
  fFormerDiscPaths.push_back(old);
  fDiscPath = rootPath.Path();
  for (MediaItem &t : fTracks) {
    if (t.path.StartsWith(oldPrefix)) {
      t.path.Remove(0, old.Length());
      t.path.Prepend(fDiscPath);
    }
    if (t.base == old)
      t.base = fDiscPath;
  }
}

void CDRipWindow::MessageReceived(BMessage *msg) {
  switch (msg->what) {
  case kMsgFormatChanged:
  case kMsgFolderEdited:
  case kMsgPatternEdited:
  case kMsgVerifyChanged:
    _UpdatePreview();
    break;

  case kMsgToggleAll:
    // A click on the mixed state checks everything, like a plain click.
    _CheckAllTracks(fAllCheck->Value() != B_CONTROL_OFF);
    break;

  case kMsgTrackToggled:
    fLastToggle = system_time();
    _UpdateAllCheck();
    break;

  case kMsgPlayTrack:
    // A double click also cancels the edit its first click asked for.
    fLastInvoke = system_time();
    // A double click on a checkbox toggles it twice; that is not a request
    // to play.
    if (system_time() - fLastToggle > 1000000)
      _PlaySelectedTrack();
    break;

  case kMsgEditCell: {
    // Wait out the double-click time: the click may be the first of a
    // double click, which plays instead.
    bigtime_t clickSpeed = 500000;
    get_click_speed(&clickSpeed);
    fEditRequested = system_time();
    BMessage start(*msg);
    start.what = kMsgStartEdit;
    start.AddBool("delayed", true);
    delete fEditDelay;
    fEditDelay = new BMessageRunner(BMessenger(this), &start, clickSpeed, 1);
    break;
  }

  case kMsgStartEdit: {
    if (msg->GetBool("delayed", false)) {
      delete fEditDelay;
      fEditDelay = nullptr;
      if (fLastInvoke >= fEditRequested)
        break;
    }
    int32 index = -1, column = -1;
    if (msg->FindInt32("index", &index) == B_OK &&
        msg->FindInt32("column", &column) == B_OK)
      _StartEdit(index, column);
    break;
  }

  case kMsgEditDone: {
    void *editor = nullptr;
    msg->FindPointer("editor", &editor);
    if (editor != nullptr && editor == fEditor)
      _FinishEdit(msg->GetBool("commit", false), msg->GetInt32("move", 0));
    break;
  }

  case kMsgAlbumEdited:
    fAlbumDirty = true;
    _ApplyAlbumFields(false);
    break;

  case kMsgAlbumCommitted:
    _ApplyAlbumFields(true);
    break;

  case kMsgTrackMenu: {
    int32 index = -1;
    BPoint where;
    if (msg->FindInt32("index", &index) == B_OK &&
        msg->FindPoint("where", &where) == B_OK)
      _ShowTrackMenu(index, where);
    break;
  }

  case kMsgTrashTrack: {
    int32 index = -1;
    if (msg->FindInt32("index", &index) == B_OK)
      _TrashRippedFile(index);
    break;
  }

  case kMsgOffsetEdited:
    _UpdateDriveInfo();
    _UpdatePreview();
    break;

  case kMsgGiveUpEdited:
    _UpdatePreview();
    break;

  case kMsgTick:
    _ShowTrackElapsed(fTimingTrack);
    _ShowTotalElapsed();
    break;

  case MSG_CD_RIP_STATUS: {
    const char *text = nullptr;
    if (msg->FindString("text", &text) == B_OK) {
      fProgress->SetTo(fProgress->CurrentValue(), text, "");
      _ShowTotalElapsed();
    }
    // A damaged spot: shown in the track's row until its percentage moves.
    int32 track = -1;
    const char *trackStatus = nullptr;
    if (msg->FindInt32("track", &track) == B_OK &&
        msg->FindString("track_status", &trackStatus) == B_OK)
      _SetTrackStatus(track, trackStatus);
    int32 unreadable = 0;
    if (track >= 0 && msg->FindInt32("unreadable", &unreadable) == B_OK)
      _ShowLiveUnreadable(track, unreadable);
    break;
  }

  case MSG_CD_RIP_OFFSET:
    _HandleOffset(msg);
    break;

  case kMsgPreset: {
    const char *pattern = nullptr;
    if (msg->FindString("pattern", &pattern) == B_OK) {
      fPatternControl->SetText(pattern);
      _UpdatePreview();
    }
    break;
  }

  case kMsgChooseFolder:
    _ChooseFolder();
    break;

  case kMsgFolderChosen: {
    entry_ref ref;
    if (msg->FindRef("refs", &ref) == B_OK) {
      BPath path(&ref);
      if (path.InitCheck() == B_OK) {
        fFolderControl->SetText(path.Path());
        _UpdatePreview();
      }
    }
    break;
  }

  case kMsgRip:
    if (fRipper == nullptr) {
      _StartRip();
    } else {
      fRipper->Cancel();
      fRipButton->SetLabel(B_TRANSLATE("Stopping" B_UTF8_ELLIPSIS));
      fRipButton->SetEnabled(false);
      fSkipButton->SetEnabled(false);
    }
    break;

  case kMsgSkipTrack:
    // Named by index, so a click that lands as the track ends can't skip the
    // next one.
    if (fRipper != nullptr && fCurrentTrack >= 0) {
      fRipper->SkipTrack(fCurrentTrack);
      _SetTrackStatus(fCurrentTrack, B_TRANSLATE("Skipping" B_UTF8_ELLIPSIS));
    }
    break;

  case kMsgClose:
    PostMessage(B_QUIT_REQUESTED);
    break;

  case kMsgShowFolder:
    _ShowOutputFolder();
    break;

  case MSG_CD_RIP:
    // The sidebar asked for a rip window while this one is open.
    Activate();
    break;

  case MSG_CD_EJECT: {
    // The sidebar wants a disc ejected; the main window does it once this
    // window has let go of the disc.
    BString path;
    msg->FindString("path", &path);
    BMessage eject(*msg);
    eject.AddBool("rip_window_done", true);
    // Naming the disc renames its mount point; the sidebar may still have
    // the old path until it rescans.
    if (path != fDiscPath &&
        std::find(fFormerDiscPaths.begin(), fFormerDiscPaths.end(), path) ==
            fFormerDiscPaths.end()) {
      fLibraryTarget.SendMessage(&eject);
      break;
    }
    if (fRipper != nullptr && fRipper->IsRunning()) {
      Activate();
      BAlert *alert = new BAlert(
          B_TRANSLATE("Eject Disc"),
          B_TRANSLATE("Stop ripping and eject the disc?\n\nTracks that are "
                      "already finished are kept; the track being ripped now "
                      "is discarded."),
          B_TRANSLATE("Keep ripping"), B_TRANSLATE("Stop and eject"), nullptr,
          B_WIDTH_AS_USUAL, B_WARNING_ALERT);
      alert->SetShortcut(0, B_ESCAPE);
      if (alert->Go() == 0)
        break;
    }
    if (fRipper != nullptr) {
      fRipper->Cancel();
      fRipper->Wait();
      delete fRipper;
      fRipper = nullptr;
    }
    // Names edited but not saved yet go to the disc while it's still there.
    if (fEditor != nullptr)
      _FinishEdit(true, 0);
    _ApplyAlbumFields(true);
    // The disc's mount point, as it is now, is the one to eject.
    eject.RemoveName("path");
    eject.AddString("path", fDiscPath);
    _ReadControls();
    fSettings.Save();
    fLibraryTarget.SendMessage(&eject);
    Quit();
    return;
  }

  case MSG_CD_RIP_PROGRESS:
    _HandleProgress(msg);
    break;

  case MSG_CD_RIP_TRACK_DONE:
    _HandleTrackDone(msg);
    break;

  case MSG_CD_RIP_FINISHED:
    _HandleFinished(msg);
    break;

  default:
    BWindow::MessageReceived(msg);
  }
}

bool CDRipWindow::QuitRequested() {
  if (fRipper != nullptr && fRipper->IsRunning()) {
    BAlert *alert = new BAlert(
        B_TRANSLATE("Rip audio CD"),
        B_TRANSLATE("Stop ripping?\n\nTracks that are already finished are "
                    "kept; the track being ripped now is discarded."),
        B_TRANSLATE("Keep ripping"), B_TRANSLATE("Stop and close"), nullptr,
        B_WIDTH_AS_USUAL, B_WARNING_ALERT);
    alert->SetShortcut(0, B_ESCAPE);
    if (alert->Go() == 0)
      return false;
  }

  if (fRipper != nullptr) {
    fRipper->Cancel();
    fRipper->Wait();
    delete fRipper;
    fRipper = nullptr;
  }

  if (fEditor != nullptr)
    _FinishEdit(true, 0);
  _ApplyAlbumFields(true);

  _ReadControls();
  fSettings.Save();
  return true;
}
