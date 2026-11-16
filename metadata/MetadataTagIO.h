/*
 * Copyright 2005-2026, Daniel Weber.
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#ifndef BETON_METADATA_TAG_IO_H
#define BETON_METADATA_TAG_IO_H
#include "Debug.h"

#include <Locker.h>
#include <Path.h>
#include <String.h>
#include <SupportDefs.h>

#include <cmath>
#include <cstdint>
#include <vector>

/**
 * @struct TagData
 * @brief Holds metadata read from or written to audio files.
 */
struct TagData {
  BString title, artist, album, genre, comment;

  uint32 year = 0;
  uint32 track = 0;
  BString albumArtist;
  BString composer;
  uint32 trackTotal = 0;
  uint32 disc = 0;
  uint32 discTotal = 0;

  uint32 lengthSec = 0;
  uint32 bitrate = 0;
  uint32 sampleRate = 0;
  uint32 channels = 0;

  BString mbAlbumID, mbArtistID, mbTrackID;
  BString acoustId, acoustIdFp;

  uint32 rating = 0;

  /**
   * @brief Checks if any syncable fields differ from another TagData.
   * @param other The other TagData to compare.
   * @return True if there are differences worth syncing.
   */
  bool HasDifferences(const TagData &other) const {
    return title != other.title || artist != other.artist ||
           album != other.album || genre != other.genre || year != other.year ||
           track != other.track || albumArtist != other.albumArtist ||
           composer != other.composer || mbTrackID != other.mbTrackID ||
           mbAlbumID != other.mbAlbumID || mbArtistID != other.mbArtistID ||
           acoustId != other.acoustId;
  }

  /**
   * @brief Checks only Haiku's standard audio BFS attributes.
   */
  bool HasBfsStandardDifferences(const TagData &other) const {
    return title != other.title || artist != other.artist ||
           album != other.album || track != other.track ||
           year != other.year || comment != other.comment ||
           genre != other.genre || rating != other.rating ||
           lengthSec != other.lengthSec || bitrate != other.bitrate;
  }

  void LogDifferences(const TagData &other) const {
    if (title != other.title)
      DEBUG_PRINT("Diff: Title '%s' vs '%s'\n", title.String(),
                  other.title.String());
    if (artist != other.artist)
      DEBUG_PRINT("Diff: Artist '%s' vs '%s'\n", artist.String(),
                  other.artist.String());
    if (album != other.album)
      DEBUG_PRINT("Diff: Album '%s' vs '%s'\n", album.String(),
                  other.album.String());
    if (genre != other.genre)
      DEBUG_PRINT("Diff: Genre '%s' vs '%s'\n", genre.String(),
                  other.genre.String());
    if (year != other.year)
      DEBUG_PRINT("Diff: Year %u vs %u\n", (unsigned int)year,
                  (unsigned int)other.year);
    if (track != other.track)
      DEBUG_PRINT("Diff: Track %u vs %u\n", (unsigned int)track,
                  (unsigned int)other.track);
    if (albumArtist != other.albumArtist)
      DEBUG_PRINT("Diff: AlbumArtist '%s' vs '%s'\n", albumArtist.String(),
                  other.albumArtist.String());
    if (composer != other.composer)
      DEBUG_PRINT("Diff: Composer '%s' vs '%s'\n", composer.String(),
                  other.composer.String());
    if (mbTrackID != other.mbTrackID)
      DEBUG_PRINT("Diff: MBTrackID '%s' vs '%s'\n", mbTrackID.String(),
                  other.mbTrackID.String());
    if (mbAlbumID != other.mbAlbumID)
      DEBUG_PRINT("Diff: MBAlbumID '%s' vs '%s'\n", mbAlbumID.String(),
                  other.mbAlbumID.String());
    if (mbArtistID != other.mbArtistID)
      DEBUG_PRINT("Diff: MBArtistID '%s' vs '%s'\n", mbArtistID.String(),
                  other.mbArtistID.String());
    if (acoustId != other.acoustId)
      DEBUG_PRINT("Diff: AcoustID '%s' vs '%s'\n", acoustId.String(),
                  other.acoustId.String());
    if (acoustIdFp != other.acoustIdFp)
      DEBUG_PRINT("Diff: AcoustIDFp '%s' vs '%s'\n", acoustIdFp.String(),
                  other.acoustIdFp.String());
    if (comment != other.comment)
      DEBUG_PRINT("Diff: Comment '%s' vs '%s'\n", comment.String(),
                  other.comment.String());
    if (trackTotal != other.trackTotal)
      DEBUG_PRINT("Diff: TrackTotal %u vs %u\n", (unsigned int)trackTotal,
                  (unsigned int)other.trackTotal);
    if (disc != other.disc)
      DEBUG_PRINT("Diff: Disc %u vs %u\n", (unsigned int)disc,
                  (unsigned int)other.disc);
    if (discTotal != other.discTotal)
      DEBUG_PRINT("Diff: DiscTotal %u vs %u\n", (unsigned int)discTotal,
                  (unsigned int)other.discTotal);
    if (rating != other.rating)
      DEBUG_PRINT("Diff: Rating %u vs %u\n", (unsigned int)rating,
                  (unsigned int)other.rating);
  }
};

/**
 * @struct CoverBlob
 * @brief Simple container for binary cover art data.
 */
struct CoverBlob {
  std::vector<uint8_t> bytes;

  void clear() { bytes.clear(); }
  void assign(const void *p, size_t n) {
    const auto *b = static_cast<const uint8_t *>(p);
    bytes.assign(b, b + n);
  }
  const void *data() const { return bytes.empty() ? nullptr : bytes.data(); }
  size_t size() const { return bytes.size(); }
};

/**
 * @struct ReplayGainInfo
 * @brief ReplayGain 2.0 values of one file. Gains in dB relative to the
 *        -18 LUFS reference, peaks linear (1.0 = full scale); NaN = not set.
 */
struct ReplayGainInfo {
  float trackGain = NAN;
  float trackPeak = NAN;
  float albumGain = NAN;
  float albumPeak = NAN;

  bool HasTrack() const { return !std::isnan(trackGain); }
  bool HasAlbum() const { return !std::isnan(albumGain); }
};

/**
 * @struct MetadataWriteTargets
 * @brief Describes where editable text metadata should be written.
 */
struct MetadataWriteTargets {
  bool tags = false;
  bool bfs = false;
};

/**
 * @namespace MetadataTagIO
 * @brief Utilities for reading and writing audio file metadata.
 */
namespace MetadataTagIO {

/**
 * @brief Reads metadata from the specified file.
 * @param path The path to the audio file.
 * @param out Output struct to populate with metadata.
 * @param bfsFallback If true, empty tag fields fall back to the file's BFS
 *        attributes. Pass false when the result is written back to disk, or
 *        the fallback resurrects values the caller meant to clear.
 * @return True on success, false otherwise.
 */
/**
 * @brief The lock every caller must hold while inside TagLib.
 *
 * Beton links TagLib 1.x, whose ByteVector/String use implicit sharing built
 * on TagLib::RefCounter. That reference count is not safe against concurrent
 * mutation, and the shared empty/null instances are touched by every parse,
 * so two threads parsing unrelated files still race on the same counter. The
 * observed result was a double free inside ID3v2 frame parsing, with the heap
 * corruption surfacing later as faults in unrelated TagLib code.
 *
 * One scanner thread per source directory means that race is otherwise
 * routine, so every entry into TagLib -- from the scanners, the cache looper
 * and the window thread alike -- is serialised on this lock. It is recursive,
 * so nested MetadataTagIO calls are fine.
 *
 * Only the tag parsing is serialised: directory walking, stat()ing and the
 * workload counting pass all still run in parallel.
 */
BLocker &TagLibLock();

bool ReadTags(const BPath &path, TagData &out, bool bfsFallback = true);

/**
 * @brief Reads metadata from Haiku BFS attributes.
 * @param path File path.
 * @param out Output TagData structure.
 * @return True if successful, false if not on BFS or error.
 */
bool ReadBfsAttributes(const BPath &path, TagData &out);

/**
 * @brief Merges metadata from two sources based on conflict mode.
 * @param primary Primary metadata source.
 * @param secondary Secondary metadata source.
 * @param mode Conflict resolution strategy.
 * @return Merged TagData.
 */
TagData MergeMetadata(const TagData &primary, const TagData &secondary,
                      int32 mode);

/**
 * @brief Merges metadata from two sources, filling gaps where possible.
 * @param primary Primary metadata source.
 * @param secondary Secondary metadata source.
 * @param outMerged Output merged metadata.
 * @param hasConflict Set to true if a conflict (diff non-empty values) exists.
 * @return True if outMerged contains data that differs from either input.
 */
bool SmartMerge(const TagData &primary, const TagData &secondary,
                TagData &outMerged, bool &hasConflict);

/**
 * @brief Extracts embedded cover art from the file.
 * @param path The path to the audio file.
 * @param out Output struct to populate with cover data.
 * @return True if cover art was found and extracted.
 */
bool ExtractEmbeddedCover(const BPath &path, CoverBlob &out);

/**
 * @brief Writes embedded cover art to the file.
 * @param file The path to the audio file.
 * @param data Pointer to the image data.
 * @param size Size of the image data in bytes.
 * @param mimeOpt Optional MIME type string (e.g., "image/jpeg").
 * @return True on success.
 */
bool WriteEmbeddedCover(const BPath &file, const uint8_t *data, size_t size,
                        const char *mimeOpt = nullptr);

/**
 * @brief Overload that takes a CoverBlob.
 */
bool WriteEmbeddedCover(const BPath &file, const CoverBlob &blob,
                        const char *mimeOpt = nullptr);

/**
 * @brief Writes metadata and optionally cover art to the file.
 * @param path The path to the audio file.
 * @param td The new metadata to write.
 * @param coverOpt Optional pointer to cover art data to write.
 * @return True on success.
 */
bool WriteTagsToFile(const BPath &path, const TagData &td,
                     const CoverBlob *coverOpt);

/**
 * @brief Writes only metadata tags to the file.
 * @param path The path to the audio file.
 * @param in The new metadata to write.
 * @return True on success.
 */
bool WriteTags(const BPath &path, const TagData &in);

/**
 * @brief Resolves the configured metadata write targets for a file path.
 * @param filePath Absolute file path.
 * @return Enabled write targets according to MusicSourceSettings.
 */
MetadataWriteTargets WriteTargetsForPath(const BString &filePath);

/**
 * @brief Mirrors metadata and cover art to BFS attributes.
 * @param path The path to the file.
 * @param in The metadata to write as attributes.
 * @param cover Optional cover data to write as a thumbnail attribute.
 * @param coverMaxBytes Maximum allowed size for the cover attribute.
 * @return True on success.
 */
bool WriteBfsAttributes(const BPath &path, const TagData &in,
                        const CoverBlob *cover,
                        size_t coverMaxBytes = 512 * 1024);

/**
 * @brief Checks if the file resides on a BFS volume.
 * @param path The path to check.
 * @return True if on a BFS volume, false otherwise.
 */
bool IsBeFsVolume(const BPath &path);

/**
 * @brief Checks if the file is a track of a mounted audio CD.
 * @param path The path to check.
 * @return True if on a cdda volume, false otherwise.
 */
bool IsCddaVolume(const BPath &path);

/**
 * @brief Applies sync by writing source data to the appropriate destination.
 * @param path The file to sync.
 * @param source The TagData to write.
 * @param towardsBfs If true, writes to BFS attributes; if false, writes to
 * Tags.
 * @return True on success.
 */
bool ApplySync(const BPath &path, const TagData &source, bool towardsBfs);

/**
 * @brief Reads the REPLAYGAIN_* tags of a file.
 * @return True if the file has at least a track or album gain.
 */
bool ReadReplayGain(const BPath &path, ReplayGainInfo &out);

/**
 * @brief Writes REPLAYGAIN_* tags, leaving every other tag as it was.
 *
 * Only the values that are set (not NaN) are written, so a track gain can be
 * stored first and the album gain added once the whole album is measured.
 * Uses Vorbis comments (FLAC, Ogg), TXXX frames (MP3, and the ID3v2 chunk of
 * WAV) or iTunes freeform atoms (M4A) through TagLib's property interface.
 * Opus files are skipped: their gain belongs in R128_* tags and the header.
 */
bool WriteReplayGain(const BPath &path, const ReplayGainInfo &in);

} // namespace MetadataTagIO

#endif // BETON_METADATA_TAG_IO_H
