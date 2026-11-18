/*
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#ifndef BETON_RIP_NAME_PATTERN_H
#define BETON_RIP_NAME_PATTERN_H

#include "MediaItem.h"

#include <String.h>

/**
 * @namespace RipNamePattern
 * @brief Turns a pattern such as `{albumartist}/{album}/{track} - {title}`
 *        into a relative path for one ripped track.
 *
 * Fields are written in braces and are case-insensitive; spaces, `_` and `-`
 * inside the braces are ignored, so `{Album Artist}` works too. A `/` in the
 * pattern starts a subfolder. The file extension is not part of the pattern.
 *
 * | Field           | Value                                              |
 * | :-------------- | :------------------------------------------------- |
 * | `{artist}`      | Track artist                                       |
 * | `{albumartist}` | Album artist, falling back to the track artist     |
 * | `{album}`       | Album title                                        |
 * | `{title}`       | Track title                                        |
 * | `{track}`       | Track number, two digits (`01`)                    |
 * | `{tracks}`      | Number of tracks on the disc                       |
 * | `{disc}`        | Disc number, empty when unknown                    |
 * | `{year}`        | Release year, empty when unknown                   |
 * | `{genre}`       | Genre, empty when unknown                          |
 *
 * Values are made safe for file names (a `/` inside a title becomes `-`), and
 * folder or file names that end up empty are dropped.
 */
namespace RipNamePattern {

/** The fields, in the order the ripper window lists them. */
extern const char *const kFields[];
extern const int32 kFieldCount;

/**
 * @brief Expands @p pattern for @p track.
 * @param pattern The user's pattern.
 * @param track Track metadata; `trackTotal` feeds `{tracks}`.
 * @param unknownField If non-null, receives the first `{...}` that is not a
 *        known field (it is kept literally in the output).
 * @return A relative path without extension; never empty.
 */
BString Expand(const BString &pattern, const MediaItem &track,
               BString *unknownField = nullptr);

} // namespace RipNamePattern

#endif // BETON_RIP_NAME_PATTERN_H
