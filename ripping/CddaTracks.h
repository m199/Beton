/*
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#ifndef BETON_CDDA_TRACKS_H
#define BETON_CDDA_TRACKS_H

#include "MediaItem.h"

#include <String.h>
#include <vector>

/**
 * @namespace CddaTracks
 * @brief Lists the tracks of a mounted audio CD (Haiku's cdda file system).
 *
 * The cdda file system shows each track as a WAV file and puts the CD-Text or
 * CDDB names into BFS attributes. Both the CD source view and the ripper read
 * the disc through here, so they always agree on titles and track numbers.
 */
namespace CddaTracks {

/**
 * @brief Reads the tracks of the disc mounted at @p cdPath.
 * @param cdPath Mount point of the cdda volume.
 * @return Tracks sorted by track number; empty if the path can't be read.
 */
std::vector<MediaItem> Read(const BString &cdPath);

/** One entry of the disc's table of contents. */
struct TocTrack {
  uint8 number;
  bool audio; ///< False for a data track (enhanced CD).
  uint32 lba; ///< First sector, counted from the start of the program area.
};

/** The disc's table of contents. */
struct Toc {
  std::vector<TocTrack> tracks;
  uint32 leadOut = 0; ///< First sector after the last track.
};

/**
 * @brief Reads the table of contents cdda stores in the root's `CD:toc`
 *        attribute (the drive's READ TOC reply, in MSF addresses).
 */
status_t ReadToc(const BString &cdPath, Toc &out);

} // namespace CddaTracks

#endif // BETON_CDDA_TRACKS_H
