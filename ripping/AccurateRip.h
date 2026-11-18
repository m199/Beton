/*
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#ifndef BETON_ACCURATE_RIP_H
#define BETON_ACCURATE_RIP_H

#include <String.h>
#include <SupportDefs.h>

#include <map>
#include <vector>

/**
 * @namespace AccurateRip
 * @brief Checks ripped tracks against the AccurateRip database.
 *
 * AccurateRip stores, per pressing of a disc, a checksum of every track as
 * other people ripped it and how many rips agreed ("confidence"). A rip whose
 * checksum matches is bit-identical to theirs.
 *
 * Formats follow whipper's implementation (accuraterip-checksum.c, table.py,
 * accurip.py). Beton can look rips up but not submit them: the database only
 * accepts submissions from approved rippers.
 *
 * A drive reads a fixed number of samples early or late (its read offset),
 * which shifts every checksum. ListedDriveOffset() looks it up in
 * AccurateRip's list of drives; for a drive not in it, FindOffset() works the
 * offset out from the database itself. That can mislead: a disc from a
 * pressing whose audio sits some samples off the common one matches at the
 * wrong offset, which is why the list comes first.
 */
namespace AccurateRip {

/** Frames (stereo samples) AccurateRip leaves out at the start and end of
 *  the disc; any read offset below this never needs data off the disc. */
const int32 kSkipFrames = 2940;

/** The identifiers of one disc, from its table of contents. */
struct DiscId {
  uint32 id1 = 0;
  uint32 id2 = 0;
  uint32 cddb = 0;
  int32 audioTracks = 0;

  bool IsValid() const { return audioTracks > 0; }
  /** The database URL for this disc. */
  BString Url() const;
};

/**
 * @brief Builds the disc ID from the `CD:toc` (and `CD:cddbid`) attributes
 *        Haiku's cdda file system puts on the disc's root folder.
 */
status_t ReadDiscId(const BString &cdPath, DiscId &out);

/** Frames into a track where the offset-finding sector (sector 450) starts. */
const uint32 kFrame450 = 450 * 588;

/** One pressing's checksums as stored in the database. */
struct Response {
  std::vector<uint8> confidence; ///< Per audio track.
  std::vector<uint32> crc;       ///< Per audio track; v1 or v2.
  /** Per audio track: the v1-style sum over the track's sector 450 alone
   *  (multipliers 1..588), for finding a drive's offset; 0 if not stored. */
  std::vector<uint32> frame450;
};

/** Everything the database knows about one disc. */
struct Database {
  std::vector<Response> responses;

  bool IsEmpty() const { return responses.empty(); }

  /**
   * @brief Confidence of the best entry matching @p crc for @p track
   *        (0-based), or -1 when nothing matches.
   */
  int32 Match(size_t track, uint32 crc) const;

  /** As Match(), for a sector-450 checksum. */
  int32 Match450(size_t track, uint32 crc) const;
};

/** Parses a downloaded dBAR file. */
status_t Parse(const void *data, size_t length, const DiscId &id, Database &out);

/**
 * @brief Downloads the entry for @p id.
 * @return B_OK, B_ENTRY_NOT_FOUND if the disc isn't in the database, or an
 *         error with @p error set.
 */
status_t Fetch(const DiscId &id, Database &out, BString &error);

/**
 * @class Checksum
 * @brief Accumulates the v1 and v2 checksums of one track as its frames are
 *        fed in order.
 */
class Checksum {
public:
  /**
   * @param frames Length of the track in frames.
   * @param firstTrack Skip the disc's first kSkipFrames - 1 frames.
   * @param lastTrack Skip the disc's last kSkipFrames frames.
   */
  void Begin(uint32 frames, bool firstTrack, bool lastTrack);
  /** @param frames Little-endian 16-bit stereo frames as 32-bit words. */
  void Add(const uint32 *frames, size_t count);

  uint32 V1() const { return fLow; }
  uint32 V2() const { return fLow + fHigh; }

private:
  uint32 fPosition = 1; ///< 1-based frame number of the next frame.
  uint32 fFrom = 1;
  uint32 fTo = 0;
  uint32 fLow = 0;
  uint32 fHigh = 0;
};

/**
 * @brief Finds the read offset at which a track matches the database.
 *
 * Slides the v1 checksum over every offset in [-range, +range]; v1 is a
 * linear sum, so each step costs O(1).
 *
 * @param frames The track's frames with @p range extra frames on each side:
 *        frames[range] is the track's first frame at offset 0.
 * @param trackFrames Length of the track.
 * @param range Largest offset tried, at most kSkipFrames - 1.
 * @param track 0-based track index (must not be the first or last track).
 * @param db The disc's database entry.
 * @param offset Receives the offset with the most confident match.
 * @param confidence Receives that match's confidence.
 * @return True if any offset matched.
 */
bool FindOffset(const uint32 *frames, uint32 trackFrames, int32 range,
                size_t track, const Database &db, int32 &offset,
                int32 &confidence);

/**
 * @brief Finds the offsets at which a track's sector 450 matches the
 *        database: a few sectors to read instead of a whole track.
 *
 * @param frames 588 + 2 * @p range frames: frames[range] is the first frame
 *        of the track's sector 450 at offset 0.
 * @param range Largest offset tried.
 * @param track 0-based track index.
 * @param db The disc's database entry.
 * @param matches Receives offset -> confidence of the best entry matching
 *        there. Some entries are stored a few frames out, so more than one
 *        offset can match; the caller weighs them across tracks.
 */
void FindOffset450(const uint32 *frames, int32 range, size_t track,
                   const Database &db, std::map<int32, int32> &matches);

/** The drive's vendor and model ("hp CDDVDW TS-H653R"), or empty. */
BString DriveName(const BString &cdPath);

/**
 * @brief Looks a drive up in AccurateRip's list of drive read offsets.
 *
 * The list (DriveOffsets.bin, the one other rippers use) is downloaded once
 * and kept in Beton's settings folder for 30 days; an old copy is used when
 * the server can't be reached.
 *
 * @param driveName As DriveName() returns it.
 * @param offset Receives the drive's read offset.
 * @param submissions Receives how many people's rips the entry comes from.
 * @return B_OK, B_ENTRY_NOT_FOUND if the drive isn't in the list, or an error
 *         with @p error set.
 */
status_t ListedDriveOffset(const BString &driveName, int32 &offset,
                           int32 &submissions, BString &error);

/** Whether @p driveName is the drive a list entry names: "Vendor - Model"
 *  against "Vendor Model Revision", ignoring case and spacing. Exposed for
 *  tests. */
bool DriveNameMatches(const BString &driveName, const char *listName);

} // namespace AccurateRip

#endif // BETON_ACCURATE_RIP_H
