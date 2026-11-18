/*
 * Copyright 2005-2026, Daniel Weber.
 * Copyright 2026, David Beckman, indispensablelabs@gmail.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#ifndef BETON_MEDIA_TABLE_VIEW_H
#define BETON_MEDIA_TABLE_VIEW_H

#include "MediaItem.h"
#include "Messages.h"
#include <ColumnListView.h>
#include <ColumnTypes.h>
#include <MessageFilter.h>
#include <PopUpMenu.h>
#include <map>
#include <string>
#include <vector>

class CellTextControl;

/**
 * @class MediaTableView
 * @brief The main list view displaying the audio library.
 *
 * It supports:
 * Multi-column display (Title, Artist, Album, etc.).
 * Sorting by clicking column headers.
 * Drag & Drop of items.
 * Context menus.
 * Graying out missing files. (Here we still need a solution what todo with them, maybe delete from DB?)
 * 
 */
class MediaTableView : public BColumnListView {
public:
  MediaTableView(const char *name);
  virtual ~MediaTableView();

  /**
   * @brief Adds a single media item to the view.
   * @param index Row index to insert at, or -1 to append.
   */
  void AddEntry(const MediaItem &mi, int32 index = -1);

  /**
   * @brief Adds a list of items asynchronously (chunked).
   */
  void AddEntries(std::vector<MediaItem> items);

  // Serialize/deserialize a MediaItem to/from a BMessage. Used to snapshot a
  // row's data into an undo action so it can be reinserted without a rescan.
  static void ArchiveItem(const MediaItem &mi, BMessage *out);
  static MediaItem UnarchiveItem(const BMessage *in);

  void ClearEntries();
  void RefreshScrollbars();

  /**
   * @brief Builds a playback queue from the current sorted view.
   * @param[out] outQueue Vector to fill with file paths in display order.
   * @note Skips missing files.
   */
  void BuildQueue(std::vector<std::string> &outQueue) const;

  static constexpr uint32 kMsgShowCtx = MSG_SHOW_CONTEXT_MENU;

  const MediaItem *SelectedItem() const;
  const MediaItem *ItemAt(int32 index) const;
  /** The items of the rows scrolled into view, top to bottom. */
  void GetVisibleItems(std::vector<const MediaItem *> &out);
  bool HasPath(const BString &path) const;
  bool IsRowMissing(BRow *row) const;

  /**
   * @brief Sets the path of the currently playing track.
   * The row with this path will be rendered in bold.
   */
  void SetNowPlayingPath(const BString &path);
  const BString &NowPlayingPath() const { return fNowPlayingPath; }

  /**
   * @brief Switches between library and radio column layouts.
   * @param radio True to show radio columns (Name, Genre, URL),
   *              false to restore library columns.
   */
  void SetRadioMode(bool radio);

  /**
   * @brief Sets whether the view is displaying a playlist.
   * In playlist mode, the Sort column displays the sequence index.
   */
  void SetPlaylistMode(bool isPlaylist);

  /**
   * @brief Updates a single row in-place with new metadata without rebuilding the list.
   * @param mi The updated media item.
   * @param matchPath Optional row key; rows are matched against this path
   *                  instead of mi.path (used when the path itself changed).
   */
  void UpdateItem(const MediaItem &mi, const BString *matchPath = nullptr);

  void StartCellEdit(BRow *row, BColumn *column, int32 colIdx, float colLeft, BView *targetView);
  void CommitCellEdit(bool restoreFocus = true);
  void CancelCellEdit();
  const char *FieldNameForColumn(int32 colIdx) const;

  static constexpr uint32 MSG_COMMIT_EDIT = 'cmed';
  static constexpr uint32 MSG_CANCEL_EDIT = 'cned';
  static constexpr uint32 kMsgSelectAll   = 'sall';
  static constexpr uint32 kMsgLocatePlaying = 'locp';

  bool HasActiveEditor() const { return fActiveEditor != nullptr; }
  BView* ActiveEditor() const;
  void LocatePlayingTrack();

  /**
   * @brief Enables or disables inline cell editing ("Fast Edit").
   * When disabled, clicking a selected row neither opens the cell
   * editor nor registers rating-star clicks.
   */
  void SetFastEditEnabled(bool enabled) {
    if (fFastEditEnabled != enabled) {
      fFastEditEnabled = enabled;
      if (BView *outline = ScrollView())
        outline->Invalidate();
      else
        Invalidate();
    }
  }
  bool FastEditEnabled() const { return fFastEditEnabled; }

  /**
   * @brief Allows or blocks rating changes from the keyboard and context menu.
   * Off for sources whose tracks cannot take one (Radio, DLNA, Audio CD).
   */
  void SetRatingEditable(bool editable) { fRatingEditable = editable; }

  void SetFolderMode(bool folderMode) { fFolderMode = folderMode; }

  void SaveState(BMessage *msg);
  void LoadState(BMessage *msg);
  
  void SaveScrollState();

  /**
   * @brief Saves the current sort column and direction.
   * @param msg Message to store sort state into.
   * @note Delegates to BColumnListView::SaveState which includes sort info.
   */
  void SaveSortState(BMessage *msg);

  /**
   * @brief Restores a previously saved sort column and direction.
   * @param msg Message containing saved sort state.
   */
  void RestoreSortState(BMessage *msg);

protected:
  bool InitiateDrag(BPoint point, bool wasSelected) override;
  void KeyDown(const char *bytes, int32 numBytes) override;
  void MouseMoved(BPoint where, uint32 transit,
                  const BMessage *dragMsg) override;

  void AttachedToWindow() override;
  void DetachedFromWindow() override;
  void MessageReceived(BMessage *msg) override;

private:
  /** @name Filters */
  ///@{
  class RightClickFilter;
  class DropFilter;
  class EditorKeyFilter;
  class RatingKeyFilter;
  RightClickFilter *fRCFilter = nullptr;
  DropFilter *fDropFilter = nullptr;
  EditorKeyFilter *fEditorKeyFilter = nullptr;
  RatingKeyFilter *fRatingKeyFilter = nullptr;
  ///@}

  void ShowContextMenu(BPoint screenWhere);
  bool StartAdjacentCellEdit(BRow *row, int32 colIdx, int32 direction,
                             BView *targetView);
  void InstallEditorKeyFilter();
  void RemoveEditorKeyFilter();
  /**
   * @note fRowMap seemed unused in the .cpp, but keeping declaration if needed
   * later.
   */
  std::map<BRow *, MediaItem> fRowMap;

  /** @name Chunked loading state */
  ///@{
  std::vector<MediaItem> fPendingItems;
  size_t fPendingIndex = 0;
  void _AddBatch(size_t count);
  BRow* _CreateRow(const MediaItem &mi, int32 playlistIndex = -1);
  void _ApplyPendingSortRestore();
  void _PreSortPendingItems();
  static constexpr uint32 kMsgChunkAdd = 'chnk';
  BMessage fPendingSortRestore; ///< Deferred sort state to apply after last batch
  bool fHasPendingSortRestore{false};
  bool fSortingDisabledForBatch{false}; ///< True while batch loading is active
  BString fTopVisiblePath;
  std::vector<BString> fSavedSelectedPaths;
  ///@}

  /** @name Internal drag-drop reordering */
  ///@{
  int32 fDragSourceIndex = -1;
  BPoint fLastDropPoint;
  ///@}

  /** @name Now playing indicator */
  ///@{
  BString fNowPlayingPath;
  ///@}

  /** @name Display modes */
  ///@{
  bool fIsRadioMode{false};
  bool fIsPlaylistMode{false};
  std::vector<std::pair<int32, BColumn*>> fHiddenColumns; ///< Columns removed in radio mode
  std::map<int32, bool> fUserColumnVisibility;
  std::map<int32, BColumn*> fColumnByField; ///< All columns by field num, populated at construction
  ///@}

  /** @name Cell Inline Editing */
  ///@{
  bool fFastEditEnabled{false};
  bool fRatingEditable{true};
  bool fFolderMode{false};
  CellTextControl *fActiveEditor{nullptr};
  /// The row the edit began on. The library can rebuild or drop rows while
  /// an editor is open (a tag write refreshes the view), so it may be gone:
  /// read it only through _LiveEditingRow().
  BRow *fEditingRow{nullptr};
  int32 fEditingColIdx{-1};
  BString fEditingPathPrefix; ///< Volume mount point hidden from the path editor
  BString fEditingPath;     ///< The edited track's file, as the edit began
  BString fEditingOriginal; ///< The cell's text as the edit began
  BRow *fCommittedRow{nullptr}; ///< The live row of the last commit, or null
  BRow *_LiveEditingRow();
  ///@}
};

#endif // BETON_MEDIA_TABLE_VIEW_H
