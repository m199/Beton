/*
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#ifndef BETON_CD_DRIVE_READER_H
#define BETON_CD_DRIVE_READER_H

#include <String.h>
#include <SupportDefs.h>

/**
 * @class CdDriveReader
 * @brief Reads audio sectors straight from the drive, with the C2 error
 *        flags the drive raises for bytes it could not correct.
 *
 * Haiku's cdda file system reads with no error information, so a sector the
 * drive could not read cleanly comes back looking fine. This sends READ CD
 * (MMC 0xBE) through B_RAW_DEVICE_COMMAND on the drive's raw device instead,
 * which works while cdda keeps the disc mounted.
 *
 * Measured on HaikuBox (hp CDDVDW TS-H653R, hrev60368), and built in:
 * - One command moves at most 128 KiB; 49 sectors with C2 data still fit, 50
 *   are rejected. Reads go out in chunks of kChunkSectors.
 * - The drive flags (and returns uncorrected) the last ~3 sectors of a read it
 *   stops at: CD audio's error correction is interleaved across neighbouring
 *   sectors. Every read therefore asks for kOverlapSectors more than it keeps.
 * - It refuses reads past the last track ("LBA out of range"), so overlap is
 *   cut at the lead-out, and the disc's final sectors can't be verified.
 */
class CdDriveReader {
public:
  static const size_t kSectorBytes = 2352;
  /// One bit per audio byte; bit 7 of the first byte flags audio byte 0.
  static const size_t kC2Bytes = 294;
  static const uint32 kChunkSectors = 40;
  static const uint32 kOverlapSectors = 4;

  ~CdDriveReader();

  /**
   * @brief Opens the drive behind the cdda volume at @p cdPath.
   * @param leadOut First sector after the last track (reads stop there).
   * @return B_OK if the drive can be read directly and reports C2 errors.
   */
  status_t Open(const BString &cdPath, uint32 leadOut);

  /** Why Open() failed, for the rip summary. */
  const BString &ErrorText() const { return fError; }

  /**
   * @brief Reads @p count sectors from @p lba.
   * @param audio Receives count * kSectorBytes.
   * @param c2 Receives count * kC2Bytes of error flags.
   * @param overlap Read kOverlapSectors past the end; false only to test
   *        the flagging this prevents.
   * @return B_OK, or the error of the first command that failed (the
   *         caller retries; data of failed chunks is left unchanged).
   */
  status_t Read(uint32 lba, uint32 count, uint8 *audio, uint8 *c2,
                bool overlap = true);

  /** True if any of the @p count sectors' C2 flags are set. */
  static bool HasErrors(const uint8 *c2, uint32 count);

private:
  status_t _Command(uint32 lba, uint32 count, uint8 *buffer, size_t length);

  int fFd = -1;
  uint32 fLeadOut = 0;
  BString fError;
};

#endif // BETON_CD_DRIVE_READER_H
