// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * This class controls wires toolbar UI and selection model. When selection
 * status is changed, this class changes the view of toolbar. If cancel
 * selection button is pressed, this class clears the selection.
 */
class ToolbarController {
  /**
   * @param {!HTMLElement} toolbar Toolbar element which contains controls.
   * @param {!HTMLElement} navigationList Navigation list on the left pane. The
   *     position of silesSelectedLabel depends on the navitaion list's width.
   * @param {!ListContainer} listContainer List container.
   * @param {!LocationLine} locationLine Location line shown on the left side of
   *     the toolbar.
   * @param {!FileSelectionHandler} selectionHandler
   * @param {!DirectoryModel} directoryModel
   */
  constructor(
      toolbar, navigationList, listContainer, locationLine, selectionHandler,
      directoryModel) {
    /**
     * @private {!HTMLElement}
     * @const
     */
    this.toolbar_ = toolbar;

    /**
     * @private {!HTMLElement}
     * @const
     */
    this.cancelSelectionButton_ =
        queryRequiredElement('#cancel-selection-button', this.toolbar_);

    /**
     * @private {!HTMLElement}
     * @const
     */
    this.cancelSelectionButtonWrapper_ =
        queryRequiredElement('#cancel-selection-button-wrapper', this.toolbar_);

    /**
     * @private {!HTMLElement}
     * @const
     */
    this.filesSelectedLabel_ =
        queryRequiredElement('#files-selected-label', this.toolbar_);

    /**
     * @private {!HTMLElement}
     * @const
     */
    this.deleteButton_ = queryRequiredElement('#delete-button', this.toolbar_);

    /**
     * @private {!cr.ui.Command}
     * @const
     */
    this.deleteCommand_ = assertInstanceof(
        queryRequiredElement('#delete', assert(this.toolbar_.ownerDocument)),
        cr.ui.Command);

    /**
     * @private {!HTMLElement}
     * @const
     */
    this.navigationList_ = navigationList;

    /**
     * @private {!ListContainer}
     * @const
     */
    this.listContainer_ = listContainer;

    /**
     * @private {!LocationLine}
     * @const
     */
    this.locationLine_ = locationLine;

    /**
     * @private {!FileSelectionHandler}
     * @const
     */
    this.selectionHandler_ = selectionHandler;

    /**
     * @private {!DirectoryModel}
     * @const
     */
    this.directoryModel_ = directoryModel;

    this.selectionHandler_.addEventListener(
        FileSelectionHandler.EventType.CHANGE,
        this.onSelectionChanged_.bind(this));

    this.cancelSelectionButton_.addEventListener(
        'click', this.onCancelSelectionButtonClicked_.bind(this));

    this.deleteButton_.addEventListener(
        'click', this.onDeleteButtonClicked_.bind(this));

    this.navigationList_.addEventListener(
        'relayout', this.onNavigationListRelayout_.bind(this));

    // Watch visibility of toolbar buttons to update the width of location line.
    const observer =
        new MutationObserver(this.onToolbarButtonsMutated_.bind(this));
    const toolbarButtons =
        this.toolbar_.querySelectorAll('.icon-button, .combobutton');
    for (let i = 0; i < toolbarButtons.length; i++) {
      observer.observe(
          toolbarButtons[i],
          /** @type MutationObserverInit */ ({attributes: true}));
    }
  }

  /**
   * Handles selection's change event to update the UI.
   * @private
   */
  onSelectionChanged_() {
    const selection = this.selectionHandler_.selection;

    // Update the label "x files selected." on the header.
    let text;
    if (selection.totalCount === 0) {
      text = '';
    } else if (selection.totalCount === 1) {
      if (selection.directoryCount == 0) {
        text = str('ONE_FILE_SELECTED');
      } else if (selection.fileCount == 0) {
        text = str('ONE_DIRECTORY_SELECTED');
      }
    } else {
      if (selection.directoryCount == 0) {
        text = strf('MANY_FILES_SELECTED', selection.fileCount);
      } else if (selection.fileCount == 0) {
        text = strf('MANY_DIRECTORIES_SELECTED', selection.directoryCount);
      } else {
        text = strf('MANY_ENTRIES_SELECTED', selection.totalCount);
      }
    }
    this.filesSelectedLabel_.textContent = text;

    // Update visibility of the delete button.
    this.deleteButton_.hidden =
        (selection.totalCount === 0 || this.directoryModel_.isReadOnly() ||
         selection.hasReadOnlyEntry() ||
         (util.isMyFilesVolumeEnabled() &&
          this.directoryModel_.getCurrentRootType() ==
              VolumeManagerCommon.RootType.DOWNLOADS &&
          selection.entries.some(entry => entry.fullPath === '/Downloads')));

    // Set .selecting class to containing element to change the view
    // accordingly.
    // TODO(fukino): This code changes the state of body, not the toolbar, to
    // update the checkmark visibility on grid view. This should be moved to a
    // controller which controls whole app window. Or, both toolbar and FileGrid
    // should listen to the FileSelectionHandler.
    if (this.directoryModel_.getFileListSelection().multiple) {
      const bodyClassList =
          this.filesSelectedLabel_.ownerDocument.body.classList;
      bodyClassList.toggle('selecting', selection.totalCount > 0);
      if (bodyClassList.contains('check-select') !=
          /** @type {!FileListSelectionModel} */
          (this.directoryModel_.getFileListSelection()).getCheckSelectMode()) {
        bodyClassList.toggle('check-select');
        // Some custom styles depend on |check-select| class. We need to
        // re-evaluate the custom styles when the class value is changed.
        Polymer.updateStyles();
      }
    }
  }

  /**
   * Handles click event for cancel button to change the selection state.
   * @private
   */
  onCancelSelectionButtonClicked_() {
    this.directoryModel_.selectEntries([]);
  }

  /**
   * Handles click event for delete button to execute the delete command.
   * @private
   */
  onDeleteButtonClicked_() {
    this.deleteButton_.blur();
    this.deleteCommand_.canExecuteChange(this.listContainer_.currentList);
    this.deleteCommand_.execute(this.listContainer_.currentList);
  }

  /**
   * Handles the relayout event occurred on the navigation list.
   * @private
   */
  onNavigationListRelayout_() {
    // Make the width of spacer same as the width of navigation list.
    const navWidth =
        parseFloat(window.getComputedStyle(this.navigationList_).width);
    this.cancelSelectionButtonWrapper_.style.width = navWidth + 'px';
  }

  /**
   * Handles the mutation event occurred on attributes of toolbar buttons.
   * Toolbar buttons visibility can affect the available width for location
   * line.
   * @private
   */
  onToolbarButtonsMutated_() {
    this.locationLine_.truncate();
  }
}
