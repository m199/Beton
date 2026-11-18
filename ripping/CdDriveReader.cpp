/*
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "CdDriveReader.h"

#include "Debug.h"

#include <fs_info.h>
#include <scsi.h>

#include <algorithm>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <vector>

namespace {

const bigtime_t kCommandTimeout = 20000000; // 20 s: retries on a bad spot

} // namespace

CdDriveReader::~CdDriveReader() {
  if (fFd >= 0)
    close(fFd);
}

status_t CdDriveReader::Open(const BString &cdPath, uint32 leadOut) {
  fLeadOut = leadOut;
  fs_info info;
  if (fs_stat_dev(dev_for_path(cdPath.String()), &info) != B_OK) {
    fError = "the drive could not be found";
    return B_ENTRY_NOT_FOUND;
  }

  // cdda is mounted on ".../0"; commands go to the drive's "raw" device.
  char raw[B_PATH_NAME_LENGTH];
  strlcpy(raw, info.device_name, sizeof(raw));
  char *slash = strrchr(raw, '/');
  if (slash == nullptr) {
    fError = "the drive could not be found";
    return B_ENTRY_NOT_FOUND;
  }
  strlcpy(slash + 1, "raw", sizeof(raw) - (slash + 1 - raw));
  fFd = open(raw, O_RDONLY);
  if (fFd < 0) {
    fError.SetToFormat("the drive could not be opened (%s)", strerror(errno));
    return B_ERROR;
  }

  // MODE SENSE(10), capabilities page 2Ah: byte 5 bit 4 = C2 pointers.
  uint8 page[64] = {0};
  uint8 sense[32];
  raw_device_command cmd;
  memset(&cmd, 0, sizeof(cmd));
  const uint8 cdb[10] = {0x5A, 0x08, 0x2A, 0, 0, 0, 0, 0, sizeof(page), 0};
  memcpy(cmd.command, cdb, sizeof(cdb));
  cmd.command_length = sizeof(cdb);
  cmd.flags = B_RAW_DEVICE_DATA_IN | B_RAW_DEVICE_REPORT_RESIDUAL;
  cmd.data = page;
  cmd.data_length = sizeof(page);
  cmd.sense_data = sense;
  cmd.sense_data_length = sizeof(sense);
  cmd.timeout = kCommandTimeout;
  if (ioctl(fFd, B_RAW_DEVICE_COMMAND, &cmd, sizeof(cmd)) != 0 ||
      cmd.scsi_status != 0 || (page[8] & 0x3f) != 0x2A) {
    fError = "the drive did not report its capabilities";
    return B_NOT_SUPPORTED;
  }
  const uint8 *caps = page + 8; // after the mode parameter header
  if (!(caps[5] & 0x01) || !(caps[5] & 0x10)) {
    fError = "the drive does not report C2 errors";
    return B_NOT_SUPPORTED;
  }
  return B_OK;
}

status_t CdDriveReader::_Command(uint32 lba, uint32 count, uint8 *buffer,
                                 size_t length) {
  uint8 sense[32] = {0};
  raw_device_command cmd;
  memset(&cmd, 0, sizeof(cmd));
  const uint8 cdb[12] = {0xBE,
                         0x04, // expected sector type: CD-DA
                         (uint8)(lba >> 24), (uint8)(lba >> 16),
                         (uint8)(lba >> 8), (uint8)lba,
                         (uint8)(count >> 16), (uint8)(count >> 8),
                         (uint8)count,
                         0x12, // user data + C2 error pointers (294 bytes)
                         0, 0};
  memcpy(cmd.command, cdb, sizeof(cdb));
  cmd.command_length = sizeof(cdb);
  cmd.flags = B_RAW_DEVICE_DATA_IN | B_RAW_DEVICE_REPORT_RESIDUAL;
  cmd.data = buffer;
  cmd.data_length = length;
  cmd.sense_data = sense;
  cmd.sense_data_length = sizeof(sense);
  cmd.timeout = kCommandTimeout;

  if (ioctl(fFd, B_RAW_DEVICE_COMMAND, &cmd, sizeof(cmd)) != 0)
    return errno != 0 ? errno : B_IO_ERROR;
  if (cmd.scsi_status != 0) {
    DEBUG_PRINT("READ CD %lu+%lu: SCSI status %u, sense key %u, ASC %02x\n",
                (unsigned long)lba, (unsigned long)count, cmd.scsi_status,
                sense[2] & 0x0f, sense[12]);
    return B_IO_ERROR;
  }
  return B_OK;
}

status_t CdDriveReader::Read(uint32 lba, uint32 count, uint8 *audio,
                             uint8 *c2, bool overlap) {
  if (fFd < 0)
    return B_NO_INIT;

  const size_t stride = kSectorBytes + kC2Bytes;
  std::vector<uint8> buffer((kChunkSectors + kOverlapSectors) * stride);
  status_t result = B_OK;

  for (uint32 done = 0; done < count;) {
    const uint32 keep = std::min(kChunkSectors, count - done);
    const uint32 at = lba + done;
    uint32 want = keep + (overlap ? kOverlapSectors : 0);
    if (at + want > fLeadOut)
      want = fLeadOut > at ? fLeadOut - at : keep;

    status_t status = _Command(at, want, buffer.data(), want * stride);
    if (status == B_OK) {
      for (uint32 i = 0; i < keep; i++) {
        const uint8 *sector = buffer.data() + i * stride;
        memcpy(audio + (size_t)(done + i) * kSectorBytes, sector, kSectorBytes);
        memcpy(c2 + (size_t)(done + i) * kC2Bytes, sector + kSectorBytes,
               kC2Bytes);
      }
    } else if (result == B_OK) {
      result = status;
    }
    done += keep;
  }
  return result;
}

bool CdDriveReader::HasErrors(const uint8 *c2, uint32 count) {
  for (size_t i = 0; i < (size_t)count * kC2Bytes; i++)
    if (c2[i] != 0)
      return true;
  return false;
}
