/*
 * Copyright 2026, David Beckman, indispensablelabs@gmail.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "CDRipper.h"

#include "AudioFileEncoder.h"
#include "CddaTracks.h"
#include "Debug.h"
#include "Messages.h"
#include "MetadataTagIO.h"
#include "RipNamePattern.h"

#include <ByteOrder.h>
#include <Catalog.h>
#include <Directory.h>
#include <Entry.h>
#include <InterfaceDefs.h>
#include <Mime.h>
#include <Path.h>

#include <algorithm>
#include <cmath>
#include <errno.h>
#include <map>
#include <set>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "CDRipper"

namespace {

/** Frames read per call: 32 CD sectors, about 0.4 s of audio. */
const size_t kChunkFrames = 32 * 588;

const bigtime_t kProgressInterval = 200000;

/** Re-reads of a chunk whose read failed or whose sectors the drive flagged.
 *  All of them are made: on a scratched disc (Book of Love, track 10) a
 *  re-read that fixes nothing is often followed by one that does; stopping
 *  at the first miss halved the run time but left a third more sectors bad. */
const int32 kRetries = 4;

/** Time one chunk may spend on re-reads before what is still unread counts
 *  as unreadable. A drive takes seconds to give up on a sector it can't read
 *  (about 6.4 s each for a Slimtype eNAU108 on a scratched disc, traced
 *  2026-11-12), so without a limit a chunk of such sectors with all their
 *  retries could take about 7 minutes per 0.4 s of audio, and a rip sat at
 *  0% with Stop waiting for the chunk to finish. */
const bigtime_t kChunkTimeBudget = 30000000;

/** The time a chunk gets once two chunks in a row have run out of
 *  kChunkTimeBudget: a stretch that damaged gets one quick pass instead of
 *  75 s of re-reads per second of audio (a disc with damage on every second
 *  of a track, 2026-11-12). A chunk that finishes in time ends the streak. */
const bigtime_t kStreakTimeBudget = 5000000;

/** A failed read slower than this means the drive already retried it
 *  itself: reading the same thing again at once is unlikely to help. */
const bigtime_t kSlowRead = 2000000;

/** How far to move away before a re-read, so the drive reads the disc again
 *  instead of answering from its cache (about 2 MB, ~900 sectors). */
const uint32 kSeekAwaySectors = 1000;

/** Offsets tried when detecting the drive's read offset. */
const int32 kOffsetRange = AccurateRip::kSkipFrames - 1;

uint32 ReadLE32(const uint8 *p) {
  return (uint32)p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) |
         ((uint32)p[3] << 24);
}

uint16 ReadLE16(const uint8 *p) { return (uint16)(p[0] | (p[1] << 8)); }

/**
 * Finds the PCM data in a cdda track. The cdda file system always writes the
 * plain 44-byte header, but walking the chunks costs nothing. Only CD audio
 * (44.1 kHz, 16-bit stereo) is accepted: the disc is read as one continuous
 * stream of 4-byte frames across the track files.
 */
status_t ParseWav(BFile &file, off_t fileSize, off_t &dataOffset,
                  off_t &dataSize, BString &error) {
  uint8 riff[12];
  if (file.ReadAt(0, riff, sizeof(riff)) != (ssize_t)sizeof(riff) ||
      memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
    error = "Not a WAV track";
    return B_BAD_DATA;
  }

  bool haveFormat = false;
  off_t pos = 12;
  while (pos + 8 <= fileSize) {
    uint8 chunk[8];
    if (file.ReadAt(pos, chunk, sizeof(chunk)) != (ssize_t)sizeof(chunk))
      break;
    const uint32 size = ReadLE32(chunk + 4);
    const off_t body = pos + 8;

    if (memcmp(chunk, "fmt ", 4) == 0 && size >= 16) {
      uint8 fmt[16];
      if (file.ReadAt(body, fmt, sizeof(fmt)) != (ssize_t)sizeof(fmt))
        break;
      if (ReadLE16(fmt) != 1 || ReadLE16(fmt + 2) != 2 ||
          ReadLE32(fmt + 4) != 44100 || ReadLE16(fmt + 14) != 16) {
        error = "Not CD audio (44.1 kHz, 16-bit stereo)";
        return B_NOT_SUPPORTED;
      }
      haveFormat = true;
    } else if (memcmp(chunk, "data", 4) == 0) {
      if (!haveFormat)
        break;
      dataOffset = body;
      dataSize = std::min<off_t>((off_t)size, fileSize - body);
      return B_OK;
    }
    pos = body + size + (size & 1);
  }

  error = "Could not find the audio data in the track";
  return B_BAD_DATA;
}

/** "Dir/Name.flac" -> "Dir/Name (2).flac" */
BString NumberedPath(const BString &path, int32 n) {
  BString out(path);
  int32 dot = out.FindLast('.');
  int32 slash = out.FindLast('/');
  BString suffix;
  suffix << " (" << n << ")";
  if (dot > slash)
    out.Insert(suffix, dot);
  else
    out << suffix;
  return out;
}

TagData ToTagData(const MediaItem &t) {
  TagData td;
  td.title = t.title;
  td.artist = t.artist;
  td.album = t.album;
  td.albumArtist = t.albumArtist;
  td.genre = t.genre;
  td.year = t.year > 0 ? (uint32)t.year : 0;
  td.track = t.track > 0 ? (uint32)t.track : 0;
  td.trackTotal = t.trackTotal > 0 ? (uint32)t.trackTotal : 0;
  td.disc = t.disc > 0 ? (uint32)t.disc : 0;
  td.lengthSec = t.duration > 0 ? (uint32)t.duration : 0;
  return td;
}

} // namespace

CDRipper::CDRipper(const CDRipJob &job, BMessenger target)
    : fJob(job), fTarget(target) {}

CDRipper::~CDRipper() {
  Cancel();
  Wait();
}

BString CDRipper::DestinationFor(const CDRipJob &job, const MediaItem &track) {
  BString path(job.directory);
  if (!path.EndsWith("/"))
    path << '/';
  path << RipNamePattern::Expand(job.pattern, track) << '.'
       << RipFormatExtension(job.format);
  return path;
}

status_t CDRipper::Start() {
  if (fThread >= 0)
    return B_BUSY;
  fCancel = false;
  fRunning = true;
  fThread = spawn_thread(_ThreadEntry, "cd_ripper", B_LOW_PRIORITY, this);
  if (fThread < 0) {
    fRunning = false;
    return fThread;
  }
  return resume_thread(fThread);
}

void CDRipper::Cancel() { fCancel = true; }

void CDRipper::Wait() {
  if (fThread < 0)
    return;
  status_t ignored;
  wait_for_thread(fThread, &ignored);
  fThread = -1;
}

status_t CDRipper::_ThreadEntry(void *data) {
  static_cast<CDRipper *>(data)->_Run();
  return B_OK;
}

void CDRipper::_SendStatus(const char *text) {
  BMessage msg(MSG_CD_RIP_STATUS);
  msg.AddString("text", text);
  fTarget.SendMessage(&msg);
}

void CDRipper::_SendProgress(size_t index, int64 framesDone, bool force) {
  const bigtime_t now = system_time();
  if (!force && now - fLastProgress < kProgressInterval)
    return;
  fLastProgress = now;

  const int64 trackFrames = fSpans[index].frames;
  float fraction = 0.0f;
  if (fTotalFrames > 0)
    fraction = (float)(fFramesBefore + framesDone) / (float)fTotalFrames;

  BMessage msg(MSG_CD_RIP_PROGRESS);
  msg.AddInt32("track", (int32)index);
  msg.AddInt32("tracks", (int32)fJob.tracks.size());
  msg.AddFloat("fraction", std::min(fraction, 1.0f));
  msg.AddFloat("track_fraction",
               trackFrames > 0 ? (float)framesDone / (float)trackFrames : 0.0f);
  msg.AddString("title", fJob.tracks[index].title);
  msg.AddInt32("unreadable", fLiveUnreadable.load());
  // Progress is best effort: never stall the rip on a busy window.
  fTarget.SendMessage(&msg, (BHandler *)nullptr, 0);
}

void CDRipper::_SendDamageStatus(uint32 sector, uint32 done, uint32 count) {
  const bigtime_t now = system_time();
  if (done > 0 && now - fLastDamageStatus < kProgressInterval)
    return;
  fLastDamageStatus = now;

  BMessage msg(MSG_CD_RIP_STATUS);
  const int32 track = fReadingTrack.load();
  if (track >= 0 && (size_t)track < fSpans.size()) {
    // Where in the track, as a listener would find it.
    const int64 at = (int64)(sector - fFirstLba) * 588 -
                     (fSpans[track].firstFrame + fJob.offset);
    const int64 secs = std::max<int64>(0, at) / 44100;
    BString text;
    text.SetToFormat(B_TRANSLATE("Track %ld: re-reading a damaged spot at "
                                 "%ld:%02ld (sector %lu of %lu)"),
                     (long)track + 1, (long)(secs / 60), (long)(secs % 60),
                     (unsigned long)done + 1, (unsigned long)count);
    msg.AddString("text", text);
    msg.AddInt32("track", track);
    msg.AddInt32("unreadable", fLiveUnreadable.load());
    BString status;
    status.SetToFormat(B_TRANSLATE("Damaged spot at %ld:%02ld"),
                       (long)(secs / 60), (long)(secs % 60));
    msg.AddString("track_status", status);
  } else {
    msg.AddString("text", B_TRANSLATE("Re-reading a damaged spot"
                                      B_UTF8_ELLIPSIS));
  }
  // Best effort, as progress: never stall reading on a busy window.
  fTarget.SendMessage(&msg, (BHandler *)nullptr, 0);
}

status_t CDRipper::_BuildLayout(BString &error) {
  // cdda's track files are contiguous: each runs from its track's start to
  // the next one's, so the disc is simply the files one after another.
  fSpans.clear();
  fDiscFrames = 0;
  for (const MediaItem &track : fJob.tracks) {
    Span span;
    span.file.reset(new BFile(track.path.String(), B_READ_ONLY));
    off_t size = 0;
    if (span.file->InitCheck() != B_OK || span.file->GetSize(&size) != B_OK) {
      error.SetToFormat("Could not open track %ld on the disc", (long)track.track);
      return B_ERROR;
    }
    off_t dataSize = 0;
    status_t status = ParseWav(*span.file, size, span.dataOffset, dataSize, error);
    if (status != B_OK)
      return status;
    span.firstFrame = fDiscFrames;
    span.frames = dataSize / 4;
    fDiscFrames += span.frames;
    fSpans.push_back(std::move(span));
  }
  return fSpans.empty() ? B_ENTRY_NOT_FOUND : B_OK;
}

status_t CDRipper::_ReadDisc(int64 first, size_t count, uint32 *out,
                             BString &error) {
  if (fDrive) {
    _ReadDiscFromDrive(first, count, out);
    return B_OK;
  }
  return _ReadDiscFromFiles(first, count, out, error);
}

void CDRipper::_OpenDrive() {
  CddaTracks::Toc toc;
  if (CddaTracks::ReadToc(fJob.discPath, toc) != B_OK) {
    fDriveNote = B_TRANSLATE("not checked: the disc's table of contents "
                             "could not be read");
    return;
  }
  const CddaTracks::TocTrack *first = nullptr;
  for (const CddaTracks::TocTrack &t : toc.tracks) {
    if (t.audio) {
      first = &t;
      break;
    }
  }
  // Read the drive only where its sectors provably line up with cdda's
  // files: a disc with a data track (enhanced CD) doesn't, and keeps the
  // file path.
  if (first == nullptr ||
      (int64)(toc.leadOut - first->lba) * 588 != fDiscFrames) {
    fDriveNote = B_TRANSLATE("not checked: the tracks don't line up with the "
                             "disc's table of contents");
    return;
  }

  std::unique_ptr<CdDriveReader> drive(new CdDriveReader());
  if (drive->Open(fJob.discPath, toc.leadOut) != B_OK) {
    fDriveNote.SetToFormat(B_TRANSLATE("not checked: %s"),
                           drive->ErrorText().String());
    return;
  }
  fFirstLba = first->lba;
  fLeadOut = toc.leadOut;
  fDrive = std::move(drive);

  if (gIsDebug && getenv("BETON_RIP_TEST_TAILS") != nullptr)
    fTestHook = atoi(getenv("BETON_RIP_TEST_TAILS"));
  DEBUG_PRINT("CD rip: reading the drive directly, C2 checked (test hook %ld)\n",
              (long)fTestHook);
}

status_t CDRipper::_DriveRead(uint32 lba, uint32 count, uint8 *audio,
                              uint8 *c2, bool firstAttempt) {
  // Test hook: read without the protective overlap, which makes this drive
  // flag the tail of the read (1: first attempts only, 2: always), so the
  // re-read and damaged paths can be exercised without a scratched disc.
  // Only one chunk in every 2000 sectors, or a whole rip would crawl.
  const bool testChunk =
      fTestHook != 0 && (lba - fFirstLba) % 2000 < CdDriveReader::kChunkSectors;
  if (testChunk && (fTestHook == 2 || firstAttempt)) {
    for (uint32 i = 0; i < count; i += 24) {
      const uint32 n = std::min<uint32>(24, count - i);
      status_t status = fDrive->Read(lba + i, n, audio + i * CdDriveReader::kSectorBytes,
                                     c2 + i * CdDriveReader::kC2Bytes, false);
      if (status != B_OK)
        return status;
    }
    return B_OK;
  }
  return fDrive->Read(lba, count, audio, c2);
}

namespace {

const int64 kFramesPerMs = 44100 / 1000;
/** Wrong frames closer together than this sound as one glitch. */
const int64 kGlitchGapFrames = 1 * kFramesPerMs;

} // namespace

void CDRipper::_NoteGlitchLevel(int64 start, double db) {
  if (db > fStats.loudestDb) {
    fStats.loudestDb = db;
    fStats.loudestStart = start;
  }
}

void CDRipper::_NoteBadFrame(int64 frame) {
  ReadStats &s = fStats;
  s.badFrames++;
  if (s.lastBadFrame < 0 || frame - s.lastBadFrame > kGlitchGapFrames) {
    s.glitches++;
    s.runStart = frame;
  }
  s.lastBadFrame = frame;
  const int32 length = (int32)(frame - s.runStart + 1);
  if (length > s.longestFrames) {
    s.longestFrames = length;
    s.longestStart = s.runStart;
  }
}

/*
 * Graded on how much the worst glitch stands out from the music around it,
 * not how long it lasts: on Turning Keys (2026-11-11) stretches of 4-13 ms
 * that were wrong (no AccurateRip match) couldn't be heard. First guesses,
 * to be tuned by ear:
 *  - severe: any silent gap (an unreadable sector is 13 ms of silence), or a
 *    glitch 20 dB or more above the music around it (a loud click);
 *  - noticeable: 6 dB or more above it;
 *  - minor: anything the music masks.
 */
CDRipper::ErrorGrade CDRipper::GradeErrors(const ReadStats &stats,
                                           int64 trackFrames) {
  (void)trackFrames;
  if (stats.badFrames == 0)
    return kErrorsNone;
  if (stats.unreadable > 0 || stats.loudestDb >= 20.0)
    return kErrorsSevere;
  if (stats.loudestDb >= 6.0)
    return kErrorsNoticeable;
  return kErrorsMinor;
}

void CDRipper::_ReadDriveChunk(uint32 sector, uint32 count) {
  const size_t sb = CdDriveReader::kSectorBytes, cb = CdDriveReader::kC2Bytes;
  const uint32 lba = fFirstLba + sector;
  fCacheAudio.assign((size_t)count * sb, 0);
  std::vector<uint8> c2((size_t)count * cb, 0);
  // 0 = good, 1 = flagged by C2, 2 = unreadable.
  std::vector<uint8> state(count, 0);

  if (_Abandoning()) {
    // Stopping or skipping: the caller discards this, so don't touch the
    // drive, and keep the silence out of the cache (count 0: nothing cached;
    // the caller still copies this one sector out of fCacheAudio).
    fCacheSector = sector;
    fCacheCount = 0;
    return;
  }

  // Stop and Skip are checked before every drive command, so they wait for
  // one command at most, and a damaged spot gets kChunkTimeBudget (or
  // kStreakTimeBudget in a long damaged stretch) before the rest of the chunk
  // is given up as unreadable.
  const bigtime_t deadline =
      system_time() + (fDamageStreak >= 2 ? kStreakTimeBudget : kChunkTimeBudget);
  auto outOfTime = [&] { return _Abandoning() || system_time() >= deadline; };

  bigtime_t started = system_time();
  status_t status = _DriveRead(lba, count, fCacheAudio.data(), c2.data(), true);
  bool slow = system_time() - started >= kSlowRead;
  // Whole-chunk retries only for quick failures: a slow one means a sector
  // the drive already gave up on, and the salvage below finds which.
  for (int32 attempt = 0;
       status != B_OK && !slow && attempt < kRetries && !outOfTime();
       attempt++) {
    started = system_time();
    status = _DriveRead(lba, count, fCacheAudio.data(), c2.data(), false);
    slow = system_time() - started >= kSlowRead;
  }

  if (status != B_OK) {
    // The chunk as a whole keeps failing: salvage it sector by sector.
    for (uint32 i = 0; i < count; i++) {
      status_t one = B_ERROR;
      if (!outOfTime()) {
        _SendDamageStatus(lba + i, i, count);
        started = system_time();
        one = _DriveRead(lba + i, 1, fCacheAudio.data() + i * sb,
                         c2.data() + i * cb, false);
        // A second try only after a quick failure, as above.
        if (one != B_OK && system_time() - started < kSlowRead && !outOfTime())
          one = _DriveRead(lba + i, 1, fCacheAudio.data() + i * sb,
                           c2.data() + i * cb, false);
      }
      if (one != B_OK) {
        memset(fCacheAudio.data() + i * sb, 0, sb);
        memset(c2.data() + i * cb, 0, cb);
        state[i] = 2;
      }
    }
  }

  // The disc's last few sectors have nothing after them to overlap into, so
  // this drive always flags them; they are not counted as errors.
  const uint32 trusted =
      fLeadOut > CdDriveReader::kOverlapSectors
          ? fLeadOut - CdDriveReader::kOverlapSectors : 0;
  int32 flagged = 0;
  for (uint32 i = 0; i < count; i++) {
    if (state[i] == 0 && lba + i < trusted &&
        CdDriveReader::HasErrors(c2.data() + i * cb, 1)) {
      state[i] = 1;
      flagged++;
    }
  }
  fStats.flagged += flagged;

  // Re-read flagged sectors. Read somewhere else first, or the drive may
  // hand back the same bytes from its cache. C2 flags mark single bytes, so
  // each re-read fills in the bytes it got right; a sector is fixed once no
  // byte is left flagged. On Book of Love track 10 this repaired 66 of 166
  // flagged sectors in 4 re-reads, where waiting for a whole clean sector
  // repaired 46. Slowing the drive down did not help: this one (TS-H653R)
  // ignores SET CD SPEED and SET STREAMING for audio reads.
  std::vector<uint8> audio2((size_t)count * sb), c22((size_t)count * cb);
  for (int32 attempt = 0; flagged > 0 && attempt < kRetries; attempt++) {
    if (outOfTime())
      break;
    const uint32 away = lba >= kSeekAwaySectors ? lba - kSeekAwaySectors
                                                : lba + count + kSeekAwaySectors;
    if (away + 1 < fLeadOut) {
      uint8 junk[CdDriveReader::kSectorBytes], junkC2[CdDriveReader::kC2Bytes];
      fDrive->Read(away, 1, junk, junkC2);
    }
    if (_DriveRead(lba, count, audio2.data(), c22.data(), false) != B_OK)
      continue;
    for (uint32 i = 0; i < count; i++) {
      if (state[i] != 1)
        continue;
      uint8 *have = c2.data() + i * cb;
      const uint8 *got = c22.data() + i * cb;
      uint8 *data = fCacheAudio.data() + i * sb;
      const uint8 *fresh = audio2.data() + i * sb;
      for (uint32 b = 0; b < cb; b++) {
        const uint8 take = have[b] & ~got[b];
        if (take == 0)
          continue;
        for (int bit = 0; bit < 8; bit++)
          if (take & (0x80 >> bit))
            data[b * 8 + bit] = fresh[b * 8 + bit];
        have[b] &= ~take;
      }
      if (CdDriveReader::HasErrors(have, 1))
        continue;
      state[i] = 0;
      fStats.fixed++;
      flagged--;
    }
  }

  if (!_Abandoning()) {
    if (system_time() >= deadline) {
      fDamageStreak++;
      DEBUG_PRINT("CD rip: sectors %lu-%lu: out of time for re-reads "
                  "(%ld in a row)\n", (unsigned long)lba,
                  (unsigned long)(lba + count - 1), (long)fDamageStreak);
    } else {
      fDamageStreak = 0;
    }
  }

  // Measure what is left. C2 flags single bytes, so a frame (4 bytes, one
  // stereo sample) is wrong if it still holds a flagged byte; an unreadable
  // sector is wholly silent.
  const uint32 framesPerSector = (uint32)(sb / 4);
  const uint32 total = count * framesPerSector;
  std::vector<uint8> bad(total, 0);
  bool anyBad = false;
  for (uint32 i = 0; i < count; i++) {
    if (state[i] == 1) {
      fStats.damaged++;
      const uint8 *have = c2.data() + i * cb;
      for (uint32 f = 0; f < framesPerSector; f++)
        if (have[f / 2] & ((f & 1) ? 0x0F : 0xF0)) {
          bad[i * framesPerSector + f] = 1;
          anyBad = true;
        }
    } else if (state[i] == 2) {
      fStats.unreadable++;
      memset(bad.data() + i * framesPerSector, 1, framesPerSector);
      anyBad = true;
    }
  }

  fLiveUnreadable = fStats.unreadable;

  // A track this far gone isn't worth finishing: give up on it as Skip track
  // would, without waiting for the rest to be read. The user sets how far
  // (one ripped from a badly scratched disc came out more silence than
  // music, 2026-11-12); 0 gives up at the first unreadable sector.
  const int32 reading = fReadingTrack.load();
  const int32 percent = fJob.giveUpPercent;
  if (percent >= 0 && percent <= 10 && reading >= 0 &&
      (size_t)reading < fSpans.size() && fSkipTrack.load() != reading) {
    const int64 trackSectors = fSpans[reading].frames / framesPerSector;
    if (trackSectors > 0 &&
        (int64)fStats.unreadable * 100 > trackSectors * percent) {
      DEBUG_PRINT("CD rip: track %ld: %ld of %lld sectors unreadable, giving "
                  "up\n", (long)reading + 1, (long)fStats.unreadable,
                  (long long)trackSectors);
      fGaveUpTrack = reading;
      fSkipTrack = reading;
    }
  }

  if (anyBad) {
    const int64 first = (int64)sector * framesPerSector; // disc frame
    for (uint32 f = 0; f < total; f++)
      if (bad[f])
        _NoteBadFrame(first + f);

    // How audible each glitch is: its sharpest break between neighboring
    // samples (the second difference) against the roughest break in the good
    // audio within 10 ms of it. Measured within this chunk.
    auto sample = [&](int64 f, int c) {
      f = std::max<int64>(0, std::min<int64>(f, total - 1));
      const uint8 *b = fCacheAudio.data() + (size_t)f * 4 + c * 2;
      return (int32)(int16)(b[0] | (b[1] << 8));
    };
    auto sharpness = [&](uint32 f) {
      int32 most = 0;
      for (int c = 0; c < 2; c++)
        most = std::max(most, abs(2 * sample(f, c) - sample((int64)f - 1, c) -
                                  sample((int64)f + 1, c)));
      return most;
    };
    const int64 window = 10 * kFramesPerMs;
    for (uint32 f = 0; f < total;) {
      if (!bad[f]) {
        f++;
        continue;
      }
      // One glitch: wrong frames less than kGlitchGapFrames apart.
      uint32 end = f, g = f;
      while (g < total && g - end <= kGlitchGapFrames) {
        if (bad[g])
          end = g;
        g++;
      }
      int32 peak = 0;
      for (uint32 k = f; k <= end; k++)
        peak = std::max(peak, sharpness(k));
      int32 around = 0;
      const int64 from = std::max<int64>(0, (int64)f - window);
      const int64 to = std::min<int64>(total - 1, (int64)end + window);
      for (int64 k = from; k <= to; k++)
        if (!bad[k])
          around = std::max(around, sharpness((uint32)k));
      // A floor near the quietest audible level keeps silence from making
      // every tiny error look huge.
      const double db = 20.0 * log10(std::max(peak, 1) /
                                     (double)std::max(around, 64));
      _NoteGlitchLevel(first + f, db);
      f = end + 1;
    }
  }
  if (flagged > 0 || std::count(state.begin(), state.end(), 2) > 0)
    DEBUG_PRINT("CD rip: sectors %lu-%lu: %ld still flagged, %ld unreadable\n",
                (unsigned long)lba, (unsigned long)(lba + count - 1), (long)flagged,
                (long)std::count(state.begin(), state.end(), 2));

  fCacheSector = sector;
  // A chunk cut short by Stop or Skip isn't kept for the next track.
  fCacheCount = _Abandoning() ? 0 : count;
}

void CDRipper::_ReadDiscFromDrive(int64 first, size_t count, uint32 *out) {
  const int64 end = first + (int64)count;
  // Outside the disc: silence, as on the file path.
  for (int64 f = first; f < std::min<int64>(end, 0); f++)
    out[f - first] = 0;
  for (int64 f = std::max<int64>(first, fDiscFrames); f < end; f++)
    out[f - first] = 0;

  const int64 from = std::max<int64>(first, 0);
  const int64 to = std::min<int64>(end, fDiscFrames);
  if (from >= to)
    return;

  const int64 kFrames = 588; // stereo frames per sector
  for (uint32 s = (uint32)(from / kFrames); (int64)s * kFrames < to;) {
    if (fCacheCount == 0 || s < fCacheSector || s >= fCacheSector + fCacheCount) {
      const uint32 sectors = (uint32)((to + kFrames - 1) / kFrames) - s;
      _ReadDriveChunk(s, std::min(sectors, CdDriveReader::kChunkSectors));
    }
    // Copy the overlap of this cached sector with the requested frames.
    const int64 sectorStart = (int64)s * kFrames;
    const int64 a = std::max(from, sectorStart);
    const int64 b = std::min(to, sectorStart + kFrames);
    memcpy(out + (a - first),
           fCacheAudio.data() + (size_t)(s - fCacheSector) * CdDriveReader::kSectorBytes +
               (size_t)(a - sectorStart) * 4,
           (size_t)(b - a) * 4);
    s++;
  }
}

status_t CDRipper::_ReadDiscFromFiles(int64 first, size_t count, uint32 *out,
                                      BString &error) {
  size_t done = 0;
  size_t span = 0;
  while (done < count) {
    const int64 frame = first + (int64)done;
    if (frame < 0 || frame >= fDiscFrames) {
      // Before the first track or past the lead-out: cdda can't read there.
      // Silence is what other rippers use too, and AccurateRip leaves these
      // frames out of its checksums.
      out[done++] = 0;
      continue;
    }
    while (span + 1 < fSpans.size() && fSpans[span + 1].firstFrame <= frame)
      span++;
    while (span > 0 && fSpans[span].firstFrame > frame)
      span--;

    const Span &s = fSpans[span];
    const size_t n = (size_t)std::min<int64>((int64)(count - done),
                                             s.firstFrame + s.frames - frame);
    const off_t at = s.dataOffset + (frame - s.firstFrame) * 4;
    ssize_t got = s.file->ReadAt(at, out + done, n * 4);
    if (got != (ssize_t)(n * 4)) {
      const status_t status = got < 0 ? (status_t)got : B_IO_ERROR;
      error.SetToFormat("Read error at %ld:%02ld on the disc: %s",
                        (long)(frame / 44100 / 60), (long)(frame / 44100 % 60),
                        got < 0 ? strerror(status) : "unexpected end");
      return status;
    }
    done += n;
  }
  return B_OK;
}

void CDRipper::_LookUp() {
  if (!fJob.verify)
    return;

  _SendStatus(B_TRANSLATE("Looking up the disc in AccurateRip" B_UTF8_ELLIPSIS));

  AccurateRip::DiscId id;
  if (AccurateRip::ReadDiscId(fJob.discPath, id) != B_OK) {
    fLookupNote = B_TRANSLATE("not checked: the disc's table of contents "
                              "could not be read");
    return;
  }
  if (id.audioTracks != (int32)fJob.tracks.size()) {
    fLookupNote = B_TRANSLATE("not checked: the track list doesn't match the "
                              "disc's table of contents");
    return;
  }
  fDiscKey.SetToFormat("%08" B_PRIx32 "-%08" B_PRIx32 "-%08" B_PRIx32, id.id1,
                       id.id2, id.cddb);

  BString error;
  status_t status = AccurateRip::Fetch(id, fDatabase, error);
  if (status == B_OK) {
    fLookup = kRipAccurate;
  } else if (status == B_ENTRY_NOT_FOUND) {
    fLookup = kRipNotInDatabase;
    fLookupNote = B_TRANSLATE("this disc is not in the database");
  } else {
    fLookupNote.SetToFormat(B_TRANSLATE("not checked: %s"), error.String());
  }
}

void CDRipper::_ListedOffset() {
  if (fJob.offsetKnown || !fJob.verify || fJob.driveName.IsEmpty())
    return;

  _SendStatus(B_TRANSLATE("Looking up the drive in AccurateRip"
                          B_UTF8_ELLIPSIS));
  int32 offset = 0, submissions = 0;
  BString error;
  status_t status =
      AccurateRip::ListedDriveOffset(fJob.driveName, offset, submissions, error);
  if (status != B_OK) {
    DEBUG_PRINT("CD rip: %s not in AccurateRip's drive list (%s)\n",
                fJob.driveName.String(),
                status == B_ENTRY_NOT_FOUND ? "not listed" : error.String());
    return;
  }

  DEBUG_PRINT("CD rip: drive list: %s reads %+ld (%ld submissions)\n",
              fJob.driveName.String(), (long)offset, (long)submissions);
  fJob.offset = offset;
  fJob.offsetKnown = true;
  fOffsetDetected = true;
  fOffsetFromList = true;

  BMessage msg(MSG_CD_RIP_OFFSET);
  msg.AddInt32("offset", offset);
  msg.AddBool("from_list", true);
  msg.AddInt32("submissions", submissions);
  fTarget.SendMessage(&msg);
}

void CDRipper::_DetectOffset() {
  if (fJob.offsetKnown || fLookup != kRipAccurate)
    return;

  _SendStatus(B_TRANSLATE("Finding the drive's read offset" B_UTF8_ELLIPSIS));

  // Each track's sector 450 has its own checksum in the database, so a few
  // sectors per track are enough. Tracks vote: a scratched one simply
  // matches nowhere. A few entries are stored some frames out, so a track
  // can match at two offsets; the one most tracks (then the most confident
  // entries) agree on wins.
  const size_t length = 588 + 2 * (size_t)kOffsetRange;
  std::vector<uint32> frames(length);
  std::map<int32, int32> votes, weight;
  int32 read = 0;
  for (size_t i = 0; i < fSpans.size() && !fCancel; i++) {
    const Span &s = fSpans[i];
    if (s.frames < (int64)AccurateRip::kFrame450 + 588)
      continue;
    BString error;
    if (_ReadDisc(s.firstFrame + AccurateRip::kFrame450 - kOffsetRange, length,
                  frames.data(), error) != B_OK) {
      DEBUG_PRINT("CD rip: offset detection read of track %zu failed: %s\n",
                  i + 1, error.String());
      continue;
    }
    read++;
    std::map<int32, int32> matches;
    AccurateRip::FindOffset450(frames.data(), kOffsetRange, i, fDatabase,
                               matches);
    for (const auto &kv : matches) {
      votes[kv.first]++;
      weight[kv.first] += kv.second;
    }
    DEBUG_PRINT("CD rip: offset detection, track %zu: %zu offsets match\n",
                i + 1, matches.size());

    // Settled once 4 tracks agree on an offset.
    int32 top = 0;
    for (const auto &kv : votes)
      top = std::max(top, kv.second);
    if (top >= 4)
      break;
  }

  bool found = false;
  int32 offset = 0;
  for (const auto &kv : votes) {
    const int32 o = kv.first;
    if (!found || kv.second > votes[offset] ||
        (kv.second == votes[offset] &&
         (weight[o] > weight[offset] ||
          (weight[o] == weight[offset] && abs(o) < abs(offset))))) {
      offset = o;
      found = true;
    }
  }

  if (!found) {
    // Without an offset the checksums would be taken at the wrong place
    // and every track would "fail": leave AccurateRip out of this rip.
    DEBUG_PRINT("CD rip: no read offset found (%ld tracks read)\n",
                (long)read);
    fLookup = kRipNotChecked;
    BMessage msg(MSG_CD_RIP_OFFSET);
    msg.AddBool("failed", true);
    fTarget.SendMessage(&msg);
    return;
  }

  DEBUG_PRINT("CD rip: drive read offset %+ld (%ld of %ld tracks agree)\n",
              (long)offset, (long)votes[offset], (long)read);
  fJob.offset = offset;
  fJob.offsetKnown = true;
  fOffsetDetected = true;

  BMessage msg(MSG_CD_RIP_OFFSET);
  msg.AddInt32("offset", offset);
  msg.AddInt32("confidence", weight[offset]);
  fTarget.SendMessage(&msg);
}

void CDRipper::_Run() {
  int32 ripped = 0, skipped = 0, failed = 0, accurate = 0, noMatch = 0;
  int32 givenUp = 0; // Skip track
  int32 tracksWithErrors = 0;
  std::set<BString> written;
  std::vector<BString> rippedFiles;
  std::vector<double> albumBlocks;
  double albumPeak = 0;
  BMessage finished(MSG_CD_RIP_FINISHED);

  BString layoutError;
  if (_BuildLayout(layoutError) != B_OK) {
    finished.AddString("error", layoutError);
    fJob.tracks.clear();
  } else {
    _OpenDrive();
    _LookUp();
    if (!fCancel)
      _ListedOffset();
    if (!fCancel)
      _DetectOffset();
  }

  auto isSelected = [this](size_t i) {
    return fJob.selected.empty() || (i < fJob.selected.size() && fJob.selected[i]);
  };
  for (size_t i = 0; i < fSpans.size(); i++) {
    if (isSelected(i))
      fTotalFrames += fSpans[i].frames;
  }

  for (size_t i = 0; i < fJob.tracks.size() && !fCancel; i++) {
    // Unselected tracks are left out; the window already marked them.
    if (!isSelected(i))
      continue;
    const MediaItem &track = fJob.tracks[i];
    BString dest = DestinationFor(fJob, track);

    // Two tracks resolving to one name (a pattern without {track} and two
    // untitled tracks): number the later ones instead of skipping them.
    for (int32 n = 2; written.count(dest) > 0; n++)
      dest = NumberedPath(DestinationFor(fJob, track), n);

    // Reset before the first progress message, which carries the count:
    // otherwise the last track's unreadable sectors showed on this one's row.
    fStats = ReadStats();
    fLiveUnreadable = 0;
    _SendProgress(i, 0, true);

    BString error;
    status_t status;
    RipVerdict verdict = kRipNotChecked;
    int32 confidence = -1;
    AccurateRip::Checksum checksum;
    LoudnessMeter loudness(44100, 2);

    if (BEntry(dest.String()).Exists()) {
      status = B_FILE_EXISTS;
      skipped++;
    } else {
      status = _RipTrack(i, dest, checksum, loudness, error);
      if (status == B_OK) {
        ripped++;
        written.insert(dest);
        finished.AddString("path", dest);
        rippedFiles.push_back(dest);
        albumBlocks.insert(albumBlocks.end(), loudness.Blocks().begin(),
                           loudness.Blocks().end());
        albumPeak = std::max(albumPeak, loudness.Peak());

        if (fLookup == kRipAccurate) {
          confidence = std::max(fDatabase.Match(i, checksum.V2()),
                                fDatabase.Match(i, checksum.V1()));
          verdict = confidence >= 0 ? kRipAccurate : kRipNoMatch;
          if (verdict == kRipAccurate)
            accurate++;
          else
            noMatch++;
        } else if (fLookup == kRipNotInDatabase) {
          verdict = kRipNotInDatabase;
        }
        DEBUG_PRINT("CD rip: track %zu v1 %08" B_PRIx32 " v2 %08" B_PRIx32
                    " -> %d (%ld)\n", i + 1, checksum.V1(), checksum.V2(),
                    (int)verdict, (long)confidence);
      } else if (status == B_INTERRUPTED) {
        givenUp++;
      } else if (status != B_CANCELED) {
        failed++;
      }
    }
    fFramesBefore += fSpans[i].frames;

    if (status == B_CANCELED)
      break;

    BMessage done(MSG_CD_RIP_TRACK_DONE);
    done.AddInt32("track", (int32)i);
    done.AddInt32("status", status);
    done.AddString("path", dest);
    done.AddString("title", track.title);
    done.AddString("error", error);
    done.AddInt32("ar", verdict);
    done.AddInt32("ar_confidence", confidence);
    const ErrorGrade grade = GradeErrors(fStats, fSpans[i].frames);
    if (fDrive && status == B_OK)
      DEBUG_PRINT("CD rip: track %zu read errors: %ld flagged, %ld fixed, "
                  "%ld damaged, %ld unreadable (sectors); %lld frames wrong in "
                  "%ld glitches, longest %ld frames (%.1f ms), worst stands "
                  "out %.1f dB; grade %d\n",
                  i + 1, (long)fStats.flagged, (long)fStats.fixed,
                  (long)fStats.damaged, (long)fStats.unreadable,
                  (long long)fStats.badFrames, (long)fStats.glitches,
                  (long)fStats.longestFrames,
                  fStats.longestFrames / (double)kFramesPerMs,
                  fStats.loudestDb, (int)grade);
    done.AddBool("errors_checked", fDrive != nullptr && status == B_OK);
    done.AddInt32("err_flagged", fStats.flagged);
    done.AddInt32("err_fixed", fStats.fixed);
    done.AddInt32("err_damaged", fStats.damaged);
    done.AddInt32("err_unreadable", fStats.unreadable);
    done.AddInt32("err_glitches", fStats.glitches);
    done.AddInt32("err_longest_frames", fStats.longestFrames);
    // The loudest glitch: how loud, and where in the track, to listen to it.
    if (fStats.glitches > 0) {
      const int64 at =
          fStats.loudestStart - (fSpans[i].firstFrame + fJob.offset);
      done.AddInt64("err_loudest_at_ms", std::max<int64>(0, at) * 1000 / 44100);
      done.AddFloat("err_loudest_db", (float)fStats.loudestDb);
    }
    done.AddInt32("err_grade", grade);
    done.AddBool("gave_up", fGaveUpTrack.load() == (int32)i);
    done.AddInt32("give_up_percent", fJob.giveUpPercent);
    // Worth a warning: errors a listener may hear. An AccurateRip match
    // proves the audio exact, so the flagged bytes were right after all.
    if (grade >= kErrorsNoticeable && verdict != kRipAccurate)
      tracksWithErrors++;
    if (status == B_OK && std::isfinite(loudness.Loudness()))
      done.AddFloat("rg_gain", (float)loudness.Gain());
    fTarget.SendMessage(&done);
  }

  // Album gain is only meaningful for the whole album, so only a rip that
  // covered every track gets it.
  if (!fCancel && ripped > 0 && ripped == (int32)fJob.tracks.size())
    _WriteAlbumGain(rippedFiles, albumBlocks, albumPeak);

  finished.AddInt32("ripped", ripped);
  finished.AddInt32("skipped", skipped);
  finished.AddInt32("given_up", givenUp);
  finished.AddInt32("failed", failed);
  finished.AddInt32("accurate", accurate);
  finished.AddInt32("no_match", noMatch);
  finished.AddString("ar_note", fLookupNote);
  finished.AddBool("errors_checked", fDrive != nullptr);
  finished.AddString("errors_note", fDriveNote);
  finished.AddInt32("tracks_with_errors", tracksWithErrors);
  finished.AddBool("offset_known", fJob.offsetKnown);
  finished.AddBool("offset_detected", fOffsetDetected);
  finished.AddBool("offset_from_list", fOffsetFromList);
  finished.AddInt32("offset", fJob.offset);
  finished.AddString("disc_key", fDiscKey);
  finished.AddBool("cancelled", fCancel.load());
  fRunning = false;
  fTarget.SendMessage(&finished);
}

/**
 * The disc is read on a second thread while this one encodes: reading mostly
 * waits on the drive and encoding (MP3 above all) mostly uses the CPU, so the
 * two overlap. Chunks pass through a ring of kSlots buffers; the reader stops
 * at the first error, marking it with an empty chunk.
 */
struct CDRipper::ReadAhead {
  static const int32 kSlots = 16; // ~6 s of audio

  CDRipper *ripper = nullptr;
  int64 first = 0;  ///< Disc frame the track starts at (offset applied).
  int64 frames = 0;
  std::vector<uint32> buffers[kSlots];
  size_t counts[kSlots] = {};
  sem_id free = -1;   ///< Slots the reader may fill.
  sem_id filled = -1; ///< Slots the encoder may take.
  std::atomic<bool> stop{false};
  status_t status = B_OK; ///< The reader's error, valid after an empty chunk.
  BString error;
};

status_t CDRipper::_ReadAheadEntry(void *data) {
  ReadAhead &ra = *static_cast<ReadAhead *>(data);
  int32 slot = 0;
  for (int64 done = 0; done < ra.frames; slot = (slot + 1) % ReadAhead::kSlots) {
    if (acquire_sem(ra.free) != B_OK || ra.stop || ra.ripper->_Abandoning())
      break;
    const size_t n = (size_t)std::min<int64>(kChunkFrames, ra.frames - done);
    ra.status = ra.ripper->_ReadDisc(ra.first + done, n,
                                     ra.buffers[slot].data(), ra.error);
    ra.counts[slot] = ra.status == B_OK ? n : 0;
    release_sem(ra.filled);
    if (ra.status != B_OK)
      break;
    done += n;
  }
  return B_OK;
}

status_t CDRipper::_RipTrack(size_t index, const BString &dest,
                             AccurateRip::Checksum &checksum,
                             LoudnessMeter &loudness, BString &error) {
  const MediaItem &track = fJob.tracks[index];
  const Span &span = fSpans[index];

  BPath destPath(dest.String());
  BPath parent;
  if (destPath.InitCheck() != B_OK || destPath.GetParent(&parent) != B_OK) {
    error = "Invalid destination path";
    return B_BAD_VALUE;
  }
  status_t status = create_directory(parent.Path(), 0755);
  if (status != B_OK) {
    error.SetToFormat("Could not create folder %s: %s", parent.Path(),
                      strerror(status));
    return status;
  }

  BString partPath(dest);
  partPath << ".part";

  std::unique_ptr<AudioFileEncoder> encoder(CreateAudioFileEncoder(fJob.format));
  status = encoder->Open(partPath.String(), 44100, 2, track);
  if (status != B_OK) {
    error = encoder->ErrorText();
    unlink(partPath.String());
    return status;
  }

  checksum.Begin((uint32)span.frames, index == 0,
                 index + 1 == fSpans.size());

  // The track as other rippers see it: shifted by the drive's read offset,
  // which may reach a little into the neighbouring tracks.
  ReadAhead ra;
  ra.ripper = this;
  ra.first = span.firstFrame + fJob.offset;
  ra.frames = span.frames;
  for (std::vector<uint32> &b : ra.buffers)
    b.resize(kChunkFrames);
  ra.free = create_sem(ReadAhead::kSlots, "rip read slots");
  ra.filled = create_sem(0, "rip read chunks");
  thread_id reader = -1;
  if (ra.free >= B_OK && ra.filled >= B_OK)
    reader = spawn_thread(_ReadAheadEntry, "rip reader", B_NORMAL_PRIORITY, &ra);
  if (reader < B_OK) {
    delete_sem(ra.free);
    delete_sem(ra.filled);
    encoder->Abort();
    unlink(partPath.String());
    error = "Could not start reading the disc";
    return B_NO_MEMORY;
  }
  fDamageStreak = 0;
  fReadingTrack = (int32)index;
  resume_thread(reader);

  int64 done = 0;
  int32 slot = 0;
  status = B_OK;
  while (done < span.frames) {
    if (fCancel) {
      status = B_CANCELED;
      break;
    }
    if (fSkipTrack.load() == (int32)index) {
      status = B_INTERRUPTED;
      error = fGaveUpTrack.load() == (int32)index
                  ? B_TRANSLATE("Too damaged")
                  : B_TRANSLATE("Skipped");
      break;
    }
    if (acquire_sem(ra.filled) != B_OK) {
      status = B_ERROR;
      break;
    }
    const size_t n = ra.counts[slot];
    if (n == 0) {
      status = ra.status;
      error = ra.error;
      break;
    }
    uint32 *buffer = ra.buffers[slot].data();
    checksum.Add(buffer, n);

#if B_HOST_IS_BENDIAN
    swap_data(B_INT16_TYPE, buffer, n * 4, B_SWAP_LENDIAN_TO_HOST);
#endif
    loudness.AddFrames(reinterpret_cast<const int16 *>(buffer), n);
    status = encoder->Write(reinterpret_cast<const int16 *>(buffer), (int32)n);
    if (status != B_OK) {
      error = encoder->ErrorText();
      break;
    }
    release_sem(ra.free);
    slot = (slot + 1) % ReadAhead::kSlots;
    done += n;
    _SendProgress(index, done, false);
  }

  // Deleting the semaphores wakes a reader waiting for a free slot.
  ra.stop = true;
  delete_sem(ra.free);
  delete_sem(ra.filled);
  status_t ignored;
  wait_for_thread(reader, &ignored);
  fReadingTrack = -1;

  if (status != B_OK) {
    encoder->Abort();
    unlink(partPath.String());
    return status;
  }

  status = encoder->Finish();
  if (status != B_OK) {
    error = encoder->ErrorText();
    unlink(partPath.String());
    return status;
  }

  if (rename(partPath.String(), dest.String()) != 0) {
    error.SetToFormat("Could not rename the finished file: %s", strerror(errno));
    unlink(partPath.String());
    return B_ERROR;
  }

  _FinishFile(dest, track);

  if (std::isfinite(loudness.Loudness())) {
    ReplayGainInfo gain;
    gain.trackGain = (float)loudness.Gain();
    gain.trackPeak = (float)loudness.Peak();
    if (!MetadataTagIO::WriteReplayGain(BPath(dest.String()), gain))
      DEBUG_PRINT("CD rip: could not write ReplayGain to %s\n", dest.String());
  }
  return B_OK;
}

void CDRipper::_WriteAlbumGain(const std::vector<BString> &files,
                               const std::vector<double> &blocks, double peak) {
  const double lufs = LoudnessMeter::GatedLoudness(blocks);
  if (!std::isfinite(lufs))
    return;
  ReplayGainInfo gain;
  gain.albumGain = (float)LoudnessMeter::GainFor(lufs);
  gain.albumPeak = (float)peak;
  DEBUG_PRINT("CD rip: album loudness %.2f LUFS, gain %+.2f dB, peak %.4f\n",
              lufs, gain.albumGain, peak);
  for (const BString &file : files)
    MetadataTagIO::WriteReplayGain(BPath(file.String()), gain);
}

void CDRipper::_FinishFile(const BString &dest, const MediaItem &track) {
  BPath path(dest.String());

  // FFmpeg tags a WAV with a RIFF INFO list only. TagLib 1.13 parses that,
  // but its combined tag() -- what Beton's reader uses -- comes back empty
  // for an INFO-only WAV (seen with taglib 1.13 on hrev60368). Writing the
  // tags again through TagLib adds an ID3v2 chunk, which it does read.
  if (fJob.format == RipFormat::WAV) {
    if (!MetadataTagIO::WriteTagsToFile(path, ToTagData(track), nullptr))
      DEBUG_PRINT("CD rip: could not tag %s\n", dest.String());
  }

  // Give the file its audio MIME type so Tracker and the library see it as
  // music straight away.
  update_mime_info(dest.String(), false, true, B_UPDATE_MIME_INFO_NO_FORCE);

  // Mirror the tags into Haiku's Audio:* attributes so Tracker's columns and
  // queries work without waiting for a library scan.
  if (MetadataTagIO::IsBeFsVolume(path)) {
    if (!MetadataTagIO::WriteBfsAttributes(path, ToTagData(track), nullptr))
      DEBUG_PRINT("CD rip: could not write attributes to %s\n", dest.String());
  }
}
