/*
 * Copyright 2005-2026, Daniel Weber.
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#ifndef BETON_MESSAGES_H
#define BETON_MESSAGES_H

/** @name Media Scanning & Library Management */
///@{
#define MSG_START_SCAN 'msta'       ///< Start the directory scan.
#define MSG_SCAN_DONE 'mdon'        ///< Scanning is complete.
#define MSG_SCAN_FINISHED 'scfd'    ///< Final cleanup after scan.
#define MSG_SCAN_PROGRESS 'mprg'    ///< Progress update (scanner->cache, cache->UI).
#define MSG_SCAN_TOTALS 'mtot'      ///< Scanner reports its counted workload to the cache.
#define MSG_SCAN_TICK 'mtck'        ///< UI timer: repaint scan status between updates.
#define MSG_MEDIA_ITEM_FOUND 'mitm' ///< (Legacy) Single item found.
#define MSG_MEDIA_BATCH 'mbat'      ///< Batch of items from scanner to cache.
#define MSG_MEDIA_ITEM_REMOVED 'mirm' ///< Item removed from library.
#define MSG_LOAD_CACHE 'load'         ///< Request to load initial cache.
#define MSG_CACHE_LOADED 'cach'       ///< Cache loading complete.
#define MSG_RESCAN 'resc'             ///< Trigger a quick rescan.
#define MSG_RESCAN_FULL 'rscn'        ///< Trigger a full, deep rescan.
#define MSG_BASE_OFFLINE 'moff'       ///< Base path is offline/unreachable.
#define MSG_MANAGE_DIRECTORIES 'mdir' ///< Open directory manager.
#define MSG_INIT_LIBRARY 'liby'       ///< Initialize library views.
#define MSG_BATCH_TIMER 'batc'        ///< Batch update timer tick.
#define MSG_LAZY_LOAD 'lzld'          ///< Lazy loading trigger.
#define MSG_DIR_ADD 'dadd'            ///< Add directory to library.
#define MSG_DIR_REMOVE 'drmv'         ///< Remove directory from library.
#define MSG_DIR_EDIT 'dedt'           ///< Edit directory sync settings.
#define MSG_DIR_OK 'doky'             ///< Directory settings confirm.
#define MSG_ATTR_POLL 'apol' ///< Periodic poll for BFS attribute changes.
#define MSG_WATCH_FOLDER 'wtch'       ///< Watch directory node for live folder updates.
#define MSG_CACHE_FLUSH 'cfls'        ///< Throttled cache write-back tick.
#define MSG_CACHE_CORRUPT 'ccrp'      ///< Cache file was unreadable and has been discarded.
#define MSG_CACHE_EMPTY 'cmty'        ///< Cache loaded fine but holds no entries.
#define MSG_SCAN_FILES 'scfl'         ///< Queue individual file paths for (re)caching.
#define MSG_SCAN_FILES_STEP 'scfs'    ///< Internal: process the next chunk of queued files.
///@}

/** @name Playback Control */
///@{
#define MSG_PLAY 'play'           ///< Request to start playback.
#define MSG_PAUSE 'paus'          ///< Request to pause playback.
#define MSG_PLAYPAUSE 'ppau'      ///< Toggle play/pause.
#define MSG_STOP 'stop'           ///< Stop playback.
#define MSG_PLAY_NEXT 'pnxt'      ///< Play next track manually or auto.
#define MSG_NEXTS 'nxts'          ///< (Alternate) Play next.
#define MSG_PREV_SONG 'prvs'      ///< Play previous track.
#define MSG_SEEK_REQUEST 'seek'   ///< User requested seek (slider).
#define MSG_VOLUME_CHANGED 'volu' ///< Volume slider changed.
#define MSG_TIME_UPDATE 'tmuc'    ///< Periodic playback time update.
#define MSG_TRACK_ENDED 'tend'    ///< Current track finished playing.
#define MSG_NOW_PLAYING 'nply'    ///< Notification of new track playing.
#define MSG_PLAY_FAILED 'pfld'    ///< A queued track could not be opened.
#define MSG_PLAY_BTN 'plyB'       ///< Play button clicked.
#define MSG_PREV_BTN 'prvB'       ///< Previous button clicked.
#define MSG_SHUFFLE_TOGGLE 'shuf' ///< Toggle shuffle mode.
#define MSG_REPEAT_TOGGLE 'rept'  ///< Toggle repeat mode.
#define MSG_REPLAYGAIN_MODE 'rgmd' ///< Set ReplayGain playback "mode" (int32).
#define MSG_ANALYZE_LOUDNESS 'rgan' ///< Measure and tag ReplayGain for the selected tracks.
#define MSG_GAIN_PREFETCH_TICK 'rgpt' ///< Timer: hand the gain prefetcher the tracks to measure.
#define MSG_GAIN_PREFETCH_TOGGLE 'rgpm' ///< Settings: measure tracks without gain while playing.
#define MSG_GAIN_WRITE_TOGGLE 'rgpw' ///< Settings: save gain measured while playing in the files.
#define MSG_LOUDNESS_PROGRESS 'rgap' ///< Analysis thread -> window: progress/results.
///@}

/** @name UI & Selection */
///@{
#define MSG_SELECTION_CHANGED 'slch'         ///< Generic selection change.
#define MSG_SELECTION_CHANGED_CONTENT 'selC' ///< Main content list selection.
#define MSG_SELECTION_CHANGED_GENRE 'selG'   ///< Genre filter selection.
#define MSG_SELECTION_CHANGED_ARTIST 'selA'  ///< Artist filter selection.
#define MSG_SELECTION_CHANGED_ALBUM 'selB'   ///< Album filter selection.
#define MSG_DELETE_ITEM 'delI'               ///< User requested delete.
#define MSG_SHOW_CONTEXT_MENU 'ctxR'         ///< Right-click context menu.
#define MSG_REVEAL_IN_TRACKER 'rvit'         ///< Show file in Tracker.
#define MSG_ARTWORK_ON 'aron'                ///< Show artwork panel.
#define MSG_ARTWORK_OFF 'arof'               ///< Hide artwork panel.
#define MSG_FILEINFO_ON 'fion'               ///< Show file info panel.
#define MSG_FILEINFO_OFF 'fiof'              ///< Hide file info panel.
#define MSG_TOGGLE_FILTERS 'tflt'            ///< Toggle filter visibility.
#define MSG_SEARCH_MODIFY 'srch'             ///< Search query changed.
#define MSG_SEARCH_EXECUTE 'srex'            ///< Search execute (Enter).
#define MSG_UPDATE_INFO 'updI'               ///< Request to update info panel.
#define MSG_VIEW_INFO 'vifo'                 ///< View track info/properties.
#define MSG_VIEW_COVER 'vico'                ///< View cover art.
#define MSG_STATUS_UPDATE 'mstp'             ///< Update status bar text.
#define MSG_RESET_STATUS 'rstS'              ///< Clear status bar text.
#define MSG_NEXT_FILE 'next'                 ///< Navigate to next file (UI).
#define MSG_PREV_FILE 'prev'                 ///< Navigate to prev file (UI).
#define MSG_COUNT_UPDATED 'cntu'          ///< Track count updated (filtering).
#define MSG_LIBRARY_PREVIEW 'libP'        ///< Update library preview stats.
#define MSG_SEEKBAR_COLOR_DROPPED 'sbcd'  ///< Color dropped on SeekBar.
#define MSG_SELECTION_COLOR_SYSTEM 'scsy' ///< Use system selection color.
#define MSG_SELECTION_COLOR_MATCH 'scmt'  ///< Match selection to SeekBar color.
#define MSG_TOOLTIPS_ON 'tton'            ///< Enable tooltips.
#define MSG_TOOLTIPS_OFF 'ttof'           ///< Disable tooltips.
#define MSG_TOGGLE_FAST_EDIT 'tgfe'       ///< Toggle inline cell editing.
#define MSG_FILE_MOVE 'fmov'              ///< Request file move ("from"/"to" paths).
#define MSG_FILE_MOVED 'fmvd'             ///< File moved on disk; rekey cache entry.
#define MSG_FILE_MOVE_EXPECTED 'fmvx'     ///< Beton is about to move a file to "to": keep it in the library.
#define MSG_UNDO 'undo'                   ///< Undo the last edit action.
#define MSG_REDO 'redo'                   ///< Redo the last undone action.
#define MSG_VIEWS_REFRESH 'vwrf'          ///< Debounced view refresh trigger.
#define MSG_VIEWS_REFRESH_PARTIAL 'vwrp'  ///< Refresh trigger for Filters.
///@}

/** @name Playlist Management */
///@{
#define MSG_PLAYLIST_SELECTED 'plst'  ///< Playlist selected in sidebar.
#define MSG_NEW_PLAYLIST 'npls'       ///< Create new playlist request.
#define MSG_ADD_TO_PLAYLIST 'addp'    ///< Add selection to playlist.
#define MSG_LIST_PLAYLIST 'plLS'      ///< List playlists (internal).
#define MSG_DELETE_PLAYLIST 'dplt'    ///< Delete selected playlist.
#define MSG_RENAME_PLAYLIST 'rnpl'    ///< Rename selected playlist.
#define MSG_PLAYLIST_SELECTION 'selP' ///< Selection changed in playlist view.
#define MSG_PLAYLIST_CREATED 'npln'   ///< New playlist created.
#define MSG_SAVE_PLAYLIST_SELECTION 'spls' ///< Save playlist to file.
#define MSG_NEW_SMART_PLAYLIST 'nspl'  ///< Create smart/generative playlist.
#define MSG_GENERATE_PLAYLIST 'gnpl'   ///< Generate playlist from rules.
#define MSG_SET_PLAYLIST_FOLDER 'setp' ///< Set playlist storage folder.
#define MSG_PLAYLIST_FOLDER_SELECTED 'sfld' ///< Playlist folder picked.
#define MSG_NAME_PROMPT_RENAME 'nplr'       ///< Rename confirmed.
#define MSG_NAME_PROMPT_OK 'ok__'           ///< Name entry confirmed.
#define MSG_NAME_PROMPT_CANCEL 'cncl'       ///< Name entry cancelled.
#define MSG_REORDER_PLAYLIST 'rord'         ///< Reorder items in playlist.
#define MSG_PLAYLIST_ORDER_CHANGED 'plOC'   ///< Playlist sidebar order changed.
#define MSG_MOVE_TO_TRASH 'mvtr'            ///< Move selection to Trash and remove from playlist.
#define MSG_MOVE_TO 'mvto'                  ///< Prompt directory to move selected files to.
#define MSG_MOVE_TO_FOLDER_SELECTED 'mtfs'  ///< Folder picked for Move To.
#define MSG_FOLDER_ROW_RESTORE 'frrs'       ///< Undo: reinsert a moved/trashed folder row from saved data.
#define MSG_RESTORE_PLAYLIST_PATHS 'rplp'    ///< Revert/restore playlist tracks.
#define MSG_CREATE_PLAYLIST_WITH_PATHS 'cplp' ///< Create playlist and populate paths.
#define MSG_DELETE_PLAYLIST_BY_NAME 'dpln'    ///< Delete playlist by name.
///@}


/** @name Metadata & MusicBrainz */
///@{
#define MSG_MB_SEARCH 'mbsr'          ///< Start MusicBrainz search.
#define MSG_MB_SEARCH_COMPLETE 'mbsc' ///< Search results ready.
#define MSG_MB_RESULTS 'mbrs'         ///< (Alias) Results ready.
#define MSG_MB_LIST_SEL 'mbls'        ///< Result list selection.
#define MSG_MB_APPLY 'mbap'           ///< Apply single track metadata.
#define MSG_MB_APPLY_ALBUM 'mbal'     ///< Apply full album metadata.
#define MSG_MB_CANCEL 'mbcl'          ///< Cancel metadata dialog.
///@}

/** @name Properties Window */
///@{
#define MSG_PROPERTIES 'prop'          ///< Open properties window.
#define MSG_PROP_SAVE 'prsv'           ///< Save changes to file.
#define MSG_PROP_APPLY 'prap'          ///< Apply changes (no close).
#define MSG_PROP_CANCEL 'prcl'         ///< Close properties.
#define MSG_PROP_CLOSED 'prcd'         ///< Properties window closed.
#define MSG_PROP_SET_COVER_DATA 'pcvd' ///< Set cover image data.
#define MSG_PROP_REQUEST_COVER 'prcv'  ///< Request cover fetch.
#define MSG_SET_RATING 'srat'          ///< Set file rating (1-10).
///@}

/** @name Cover Art Handling */
///@{
#define MSG_COVER_LOAD 'cvld'              ///< Load cover from file/tag.
#define MSG_COVER_CLEAR 'cvcl'             ///< Clear current cover.
#define MSG_COVER_FETCH_URL 'cvul'         ///< Fetch cover from URL.
#define MSG_COVER_FETCH_MB 'cvmb'          ///< Fetch cover from MB.
#define MSG_COVER_APPLY_ALBUM 'cvba'       ///< Apply cover to whole album.
#define MSG_COVER_CLEAR_ALBUM 'cvca'       ///< Clear cover for album.
#define MSG_COVER_DROPPED_APPLY_ALL 'cvda' ///< dropped cover -> all files.
#define MSG_COVER_BITMAP_READY 'cvbr'      ///< Cover bitmap loaded & ready.
#define MSG_SHOW_COVER_CONTEXT_MENU 'sccm' ///< Show cover context menu on main screen.
#define MSG_MAIN_COVER_LOAD_REF 'mclr'     ///< Main window cover file chosen from panel.
///@}

/** @name Matching Window */
///@{
#define MSG_MATCH_APPLY 'mapl'  ///< Apply match results.
#define MSG_MATCH_CANCEL 'mcnl' ///< Cancel matching.
#define MSG_MATCH_RESULT 'mtch' ///< Match result data.
#define MSG_MOVE_UP 'mvup'      ///< Move item up.
#define MSG_MOVE_DOWN 'mvdn'    ///< Move item down.
#define MSG_SMART_MATCH 'smrt'  ///< Trigger smart matching.
#define MSG_DRAG_ITEM 'drgI'    ///< Drag started.
///@}

/** @name Debug / Misc */
///@{
#define MSG_TEST_MODE 'tstM'       ///< Trigger test mode.
#define MSG_REGISTER_TARGET 'regt' ///< Register messaging target.
///@}

/** @name Metadata Sync */
///@{
#define MSG_SYNC_SMART 'synt'           ///< Smart Sync (Bidirectional)
#define MSG_SYNC_PROGRESS 'synP'        ///< Sync progress update
#define MSG_SYNC_DONE 'synD'            ///< Sync operation complete
#define MSG_SYNC_CONFLICT 'synC'        ///< Conflict dialog requested
#define MSG_SYNC_CONFLICT_SKIP 'scsk'   ///< Skip this conflict
#define MSG_SYNC_CONFLICT_OK 'scok'     ///< Apply choice to this file
#define MSG_SYNC_CONFLICT_ALBUM 'scal'  ///< Apply choice to album
#define MSG_SYNC_CONFLICT_ALL 'scaa'    ///< Apply choice to all remaining
#define MSG_SYNC_SETTINGS_RESULT 'ssre' ///< Sync settings dialog result
/** @name Internet Radio */
///@{
#define MSG_RADIO_ADD 'radd'     ///< Add new radio station.
#define MSG_RADIO_EDIT 'redt'    ///< Edit radio station.
#define MSG_RADIO_DELETE 'rdel'  ///< Delete radio station.
#define MSG_RADIO_PLAY 'rply'    ///< Play radio station stream.
#define MSG_RADIO_IMPORT 'rimp'  ///< Import stations from m3u/pls.
#define MSG_RADIO_SAVE 'rsav'    ///< Save radio station edits.
#define MSG_STREAM_METADATA 'smta' ///< ICY/stream metadata update.
#define MSG_RADIO_FORMAT_UNSUPPORTED 'rfus' ///< Radio stream format not supported.
#define MSG_RADIO_CONNECTION_FAILED 'rfcf'  ///< Connection to radio station failed.
///@}

/** @name DLNA/UPnP */
///@{
#define MSG_DLNA_DEVICE_FOUND    'dlfd'  ///< New DLNA device discovered.
#define MSG_DLNA_DEVICE_LOST     'dlls'  ///< DLNA device timed out.
#define MSG_DLNA_SELECT_OUTPUT   'dlso'  ///< User selected output renderer.
#define MSG_DLNA_REFRESH         'dlrf'  ///< Manual discovery trigger.
#define MSG_DLNA_BROWSE          'dlbr'  ///< Browse server folder.
#define MSG_DLNA_BROWSE_DONE     'dlbd'  ///< Results of browsing a folder.
#define MSG_DLNA_SELECT_SERVER   'dlss'  ///< User selected a server from dropdown.
#define MSG_DLNA_CRAWL_PROGRESS  'dlcp'  ///< Crawl progress update (item count).
#define MSG_DLNA_CRAWL_DONE      'dlcd'  ///< Recursive crawl finished.
#define MSG_DLNA_VOLUME_UPDATE   'dlvu'  ///< DLNA renderer volume updated externally.
#define MSG_DLNA_REFRESH_CACHE   'dlrc'  ///< Invalidate and rebuild server cache.
#define MSG_DLNA_RESOURCE_UNAVAILABLE 'dlru' ///< DLNA resource cannot be played.
#define MSG_MUTE_TOGGLE          'mute'  ///< Toggle audio mute.
#define MSG_LOCAL_OUTPUT_SELECTED 'lcos' ///< User selected local output target/bus.
#define MSG_LOCAL_OUTPUT_REFRESH 'lcor'  ///< Refresh local output devices.
#define MSG_CONFLICT_POLICY_CHANGED 'cplc' ///< On System Mixer Conflict policy changed.
#define MSG_FALLBACK_DEVICE_CHANGED 'fbdc' ///< Fallback device for conflict changed.
///@}

/** @name Audio CD Ripping */
///@{
#define MSG_OPEN_IN_TRACKER 'sbOT'   ///< Sidebar menu: open the folder at "path" in Tracker.
#define MSG_CD_RIP 'cdrp'            ///< Open the rip window ("path", "label" of the disc).
#define MSG_CD_RIP_PROGRESS 'cdrP'   ///< Ripper -> window: "track", "tracks", "fraction", "title".
#define MSG_CD_RIP_TRACK_DONE 'cdrT' ///< Ripper -> window: "track", "status", "path", "error".
#define MSG_CD_RIP_FINISHED 'cdrF'   ///< Ripper -> window: counts, "cancelled", "path" per file.
#define MSG_CD_RIP_IMPORT 'cdrI'     ///< Rip window -> MainWindow: add ripped "path"s to the library; "folder" (the save folder) becomes a library folder if none holds them.
#define MSG_SIDEBAR_FOLDERS 'sbFL'   ///< Sidebar folders changed (to MainWindow); their "path"s (MainWindow -> cache).
#define MSG_LIBRARY_REMOVE 'lbRm'    ///< Remove from Library: selection (menu -> view), or "path"s (MainWindow -> cache).
#define MSG_ADD_TO_LIBRARY 'lbAD'    ///< File menu: open the Add to Library panel.
#define MSG_ADD_TO_LIBRARY_REFS 'lbAR' ///< Add "refs" (files, folders searched recursively) to the library.
#define MSG_DROP_INTO_FOLDER 'lbDF'  ///< "refs" dropped on a sidebar folder: move/copy them into "folder".
#define MSG_FOLDER_COPY_DONE 'lbDC'  ///< Copy thread -> MainWindow: "to" paths copied, "failed", "error".
#define MSG_PASTE_INTO_FOLDER 'lbPF' ///< Paste the files Tracker copied or cut into "folder".
#define MSG_PASTE_FILES 'lbPS'       ///< Edit > Paste: into the open sidebar folder, or text into the focused field.
#define MSG_REMOVE_MISSING 'lbRM'    ///< File menu: remove tracks whose files are gone.
#define MSG_PLAYLIST_SAVED 'plSv'    ///< A playlist was written; its "path"s join the library.
#define MSG_LIBRARY_RESTORE 'lbRs'   ///< Undo of a removal: put the "path"s back; "then" messages go back to the window after.
#define MSG_CD_EJECT 'cdej'          ///< Unmount and eject the disc ("path", "label"); "rip_window_done" once the rip window has let go of it.
#define MSG_CD_EJECT_FAILED 'cdeX'   ///< Eject thread -> MainWindow: "text" to show in an alert.
#define MSG_CD_RESCAN 'cdrR'         ///< Debounced: CD volumes or their files were renamed.
#define MSG_CD_RIP_PLAY 'cdrY'       ///< Rip window -> MainWindow: play ripped files ("path", "title", "artist", "album", "track", "duration" each; "index" to start at).
#define MSG_CD_RIP_STATUS 'cdrS'     ///< Ripper -> window: "text" for a step before ripping.
#define MSG_CD_RIP_OFFSET 'cdrO'     ///< Ripper -> window: detected drive "offset", "confidence".
///@}

#endif // BETON_MESSAGES_H
