/*
 * Copyright 2026, David Beckman, indispensablelabs@gmail.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#ifndef BETON_GAIN_STORE_H
#define BETON_GAIN_STORE_H

#include "MetadataTagIO.h"

#include <String.h>

/**
 * @namespace GainStore
 * @brief The library's ReplayGain values by path, readable from any thread.
 *
 * Playback reads gain from a file's tags. A gain Beton measured but did not
 * write into the file (see GainPrefetcher) lives only in the library, so
 * playback falls back to this copy of it. The main window keeps it in step
 * with its library items.
 */
namespace GainStore {

/** Stores @p info for @p path, or forgets the path when it has no gain. */
void Set(const BString &path, const ReplayGainInfo &info);
void Remove(const BString &path);
/** @return False if the library has no gain for @p path. */
bool Get(const BString &path, ReplayGainInfo &info);

} // namespace GainStore

#endif // BETON_GAIN_STORE_H
