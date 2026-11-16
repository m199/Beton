/*
 * Copyright 2005-2026, Daniel Weber.
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#ifndef BETON_SINGLE_COLUMN_LIST_VIEW_H
#define BETON_SINGLE_COLUMN_LIST_VIEW_H

#include <Message.h>
#include <Messenger.h>
#include <Rect.h>
#include <ScrollView.h>
#include <String.h>
#include <View.h>

#include <utility>
#include <vector>

/**
 * @struct SimpleItem
 * @brief Represents a single item in the SingleColumnListView.
 */
struct SimpleItem {
  BString text;          ///< The display text of the item.
  BString path;          ///< An associated hidden path or value (optional).
  bool selected = false; ///< Selection state of the item.
};

/**
 * @class SingleColumnListView
 * @brief A lightweight, custom list view; single selection, or several
 *        items when SetMultipleSelection() is on.
 *
 * This view renders a list of strings (with optional associated paths) in a
 * vertical column. It handles drawing, scrollbar updates, and mouse interaction
 * for selection. It sends a message to a target when the selection changes. And
 * the best, it works now!!!!!
 */
class SingleColumnListView : public BView {
public:
  /**
   * @brief Constructor.
   * @param name The name of the view (internal identifier).
   */
  SingleColumnListView(const char *name);

  /**
   * @brief Adds an item with the given display text.
   * @param text The text to display.
   */
  void AddItem(const BString &text);

  /**
   * @brief Adds an item with display text and an associated hidden path/value.
   * @param text The text to display.
   * @param path The associated path or value.
   */
  void AddItem(const BString &text, const BString &path);

  /**
   * @brief Removes all items from the list and clears selection.
   */
  void Clear();

  /**
   * @brief Replaces all items at once without per-item Invalidate overhead.
   * @param items Vector of {text, data} pairs.
   */
  void SetItems(const std::vector<std::pair<BString, BString>> &items);

  int32 CountItems() const;
  const BString &ItemAt(int32 index) const;
  const BString &PathAt(int32 index) const;
  /** The first selected item, or -1. */
  int32 CurrentSelection() const;
  bool IsSelected(int32 index) const;
  /** Selects @p index; unless @p extend, the others are deselected. */
  void Select(int32 index, bool extend = false);
  void Deselect(int32 index);

  /**
   * @brief Lets the user select several items: Ctrl- or Alt-click toggles
   *        one, Shift-click selects a range. Item 0 ("Show all") is never
   *        selected together with others.
   */
  void SetMultipleSelection(bool multiple);
  void RemoveItemAt(int32 index);
  void ScrollToSelection();
  void DeselectAll();

  void UpdateScrollbars();
  float LineHeight() const;

  void Draw(BRect updateRect) override;
  void FrameResized(float width, float height) override;
  void MouseDown(BPoint where) override;
  void MessageReceived(BMessage *msg) override;

  /**
   * @brief Sets the command constant for the selection change message.
   * @param what The command constant (e.g., MSG_SELECTION_CHANGED).
   */
  void SetSelectionMessage(uint32 what);

  /**
   * @brief Sets the target messenger that receives selection notifications.
   * @param target The BMessenger to send messages to.
   */
  void SetTarget(BMessenger target);

  /**
   * @brief Virtual hook called when selection changes.
   * @param index The index of the newly selected item.
   *
   * The default implementation sends the selection message to the target.
   */
  virtual void SelectionChanged(int32 index);

  /**
   * @brief Sets a custom selection background color.
   * @param color The custom selection color.
   */
  void SetSelectionColor(rgb_color color);

  /**
   * @brief Sets extra padding at the bottom of the list.
   * @param padding The amount of padding in pixels.
   */
  void SetExtraBottomPadding(float padding);

protected:
  /** @name Data */
  ///@{
  std::vector<SimpleItem> fItems;
  float fItemHeight;
  int32 fCurrentSelection;
  ///@}

  /** @name Notification */
  ///@{
  uint32 fSelectionWhat = 0;
  BMessenger fTarget;
  ///@}

  /** @name Appearance */
  ///@{
  rgb_color fSelectionColor;
  rgb_color fSelectionTextColor;
  bool fUseCustomColor = false;
  float fExtraBottomPadding = 0.0f;
  ///@}

  int32 _FirstSelected() const;

  bool fMultiple = false;
  int32 fAnchor = -1; ///< Where a Shift-click range starts.
};

#endif // BETON_SINGLE_COLUMN_LIST_VIEW_H
