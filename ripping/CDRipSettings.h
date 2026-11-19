/*
 * Copyright 2026, David Beckman, indispensablelabs@gmail.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#ifndef BETON_CD_RIP_SETTINGS_H
#define BETON_CD_RIP_SETTINGS_H

#include <Message.h>
#include <String.h>
#include <SupportDefs.h>

/** Output formats offered by the CD ripper. Values are stored in settings. */
enum class RipFormat : int32 {
  MP3_320 = 0, ///< MP3, 320 kbps CBR (LAME).
  FLAC = 1,    ///< FLAC, lossless.
  WAV = 2      ///< WAV, 16-bit PCM, uncompressed.
};

/** File name extension for @p format, without the dot. */
const char *RipFormatExtension(RipFormat format);

/**
 * @struct CDRipSettings
 * @brief The ripper's remembered choices, kept in
 *        `~/config/settings/Beton/cd_rip.settings`.
 */
struct CDRipSettings {
  RipFormat format = RipFormat::FLAC;
  BString directory; ///< Absolute folder the pattern is resolved against.
  BString pattern;   ///< Folder/file name pattern, see RipNamePattern.
  bool verify = true; ///< Check rips against the AccurateRip database.
  /** Add ripped tracks to the library, adding the save folder to the
   *  library's folders if it isn't in one already. */
  bool addToLibrary = true;
  /** Give up on a track once more than this percentage of its sectors are
   *  unreadable: 0 (the first one) to 10. -1: never, only Skip track. */
  int32 giveUpPercent = -1;

  /**
   * @brief The remembered read offset of a drive.
   * @param drive Drive name from AccurateRip::DriveName().
   * @return False if the offset of this drive isn't known yet.
   */
  bool DriveOffset(const BString &drive, int32 &offset) const;
  void SetDriveOffset(const BString &drive, int32 offset);
  void ForgetDriveOffset(const BString &drive);

  /**
   * @brief An offset one disc found for a drive, waiting for a second disc
   *        to agree before it is kept as the drive's offset.
   * @param disc Identifies the disc that found it (AccurateRip disc ID).
   */
  bool OffsetCandidate(const BString &drive, int32 &offset,
                       BString &disc) const;
  void SetOffsetCandidate(const BString &drive, int32 offset,
                          const BString &disc);
  void ForgetOffsetCandidate(const BString &drive);

  /** Loads saved settings, filling anything missing with defaults. */
  void Load();
  /** Writes the settings back to disk. */
  void Save() const;

  /** The pattern used when none was saved. */
  static const char *DefaultPattern();
  /** First library folder, or ~/music when no library folder is set up. */
  static BString DefaultDirectory();

private:
  BMessage fDriveOffsets; ///< Drive name -> int32 read offset in samples.
  /** Drive name -> message with "offset" (int32) and "disc" (string). */
  BMessage fOffsetCandidates;
};

#endif // BETON_CD_RIP_SETTINGS_H
