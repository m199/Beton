/*
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "AccurateRip.h"

#include "CddaTracks.h"
#include "Debug.h"

#include <ByteOrder.h>
#include <DataIO.h>
#include <Drivers.h>
#include <File.h>
#include <FindDirectory.h>
#include <Node.h>
#include <Path.h>
#include <Url.h>
#include <UrlProtocolRoster.h>
#include <fs_info.h>
#include <private/netservices/HttpRequest.h>
#include <private/netservices/HttpResult.h>

#include <ctype.h>
#include <fcntl.h>
#include <memory>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

using namespace BPrivate::Network;

namespace {

uint32 ReadLE32(const uint8 *p) {
  return (uint32)p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) |
         ((uint32)p[3] << 24);
}

/**
 * Downloads @p url into @p sink. Returns B_OK with the HTTP status in
 * @p httpStatus (0 if no answer came), or an error with @p error set.
 */
status_t HttpGet(const BString &urlText, BMallocIO &sink, int32 &httpStatus,
                 BString &error) {
  httpStatus = 0;
  try {
#if B_HAIKU_VERSION <= B_HAIKU_VERSION_1_BETA_5
    BUrl url(urlText.String());
#else
    BUrl url(urlText.String(), true);
#endif
    std::unique_ptr<BUrlRequest> request(
        BUrlProtocolRoster::MakeRequest(url, &sink));
    if (!request) {
      error = "Could not create the request";
      return B_ERROR;
    }
    if (auto http = dynamic_cast<BHttpRequest *>(request.get())) {
      http->SetUserAgent("Beton");
      http->SetFollowLocation(true);
    }

    thread_id thread = request->Run();
    if (thread < 0) {
      error = "Could not start the request";
      return thread;
    }
    status_t exit;
    if (wait_for_thread_etc(thread, B_RELATIVE_TIMEOUT, 20000000, &exit) !=
        B_OK) {
      request->Stop();
      wait_for_thread_etc(thread, B_RELATIVE_TIMEOUT, 2000000, &exit);
      error = "The AccurateRip server did not answer";
      return B_TIMED_OUT;
    }

    const BHttpResult *result =
        dynamic_cast<const BHttpResult *>(&request->Result());
    httpStatus = result ? result->StatusCode() : 0;
    DEBUG_PRINT("AccurateRip: %s -> HTTP %ld, %zu bytes\n", urlText.String(),
                (long)httpStatus, (size_t)sink.BufferLength());
    return B_OK;
  } catch (...) {
    error = "The AccurateRip lookup failed";
    return B_ERROR;
  }
}

/** Lower case, runs of spaces as one, no spaces at either end. */
BString NormalizedName(const char *text, size_t length) {
  BString out;
  bool space = false;
  for (size_t i = 0; i < length && text[i] != '\0'; i++) {
    const char c = text[i];
    if (isspace((unsigned char)c)) {
      space = !out.IsEmpty();
      continue;
    }
    if (space)
      out << ' ';
    space = false;
    out << (char)tolower((unsigned char)c);
  }
  return out;
}

const char *kDriveListUrl =
    "http://www.accuraterip.com/accuraterip/DriveOffsets.bin";
const char *kDriveListFile = "Beton/DriveOffsets.bin";
/** Each entry: offset (int16), "Vendor - Model" (33 bytes, NUL padded),
 *  submissions (uint32 at byte 35), then fields Beton doesn't use. */
const size_t kDriveRecord = 69;
const bigtime_t kDriveListMaxAge = 30LL * 24 * 3600 * 1000000;

/** The cached list, downloading it when missing or old. */
status_t LoadDriveList(BMallocIO &list, BString &error) {
  BPath path;
  if (find_directory(B_USER_SETTINGS_DIRECTORY, &path, true) != B_OK) {
    error = "No settings folder";
    return B_ERROR;
  }
  path.Append(kDriveListFile);

  BFile cached(path.Path(), B_READ_ONLY);
  off_t size = 0;
  time_t modified = 0;
  const bool haveCache = cached.InitCheck() == B_OK &&
                         cached.GetSize(&size) == B_OK && size > 0 &&
                         size % kDriveRecord == 0 &&
                         cached.GetModificationTime(&modified) == B_OK;
  const bool fresh =
      haveCache &&
      (bigtime_t)(real_time_clock() - modified) * 1000000 < kDriveListMaxAge;

  if (!fresh) {
    BMallocIO download;
    int32 http = 0;
    BString fetchError;
    if (HttpGet(kDriveListUrl, download, http, fetchError) == B_OK &&
        http == 200 && download.BufferLength() > 0 &&
        download.BufferLength() % kDriveRecord == 0) {
      // Written beside the old copy and renamed, so a failed write never
      // leaves half a list.
      BString partial(path.Path());
      partial << ".part";
      BFile out(partial.String(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
      if (out.InitCheck() == B_OK &&
          out.Write(download.Buffer(), download.BufferLength()) ==
              (ssize_t)download.BufferLength()) {
        out.Unset();
        rename(partial.String(), path.Path());
      } else {
        unlink(partial.String());
      }
      list.Write(download.Buffer(), download.BufferLength());
      return B_OK;
    }
    if (!haveCache) {
      error = fetchError.IsEmpty()
                  ? BString("The AccurateRip drive list could not be read")
                  : fetchError;
      return B_ERROR;
    }
    DEBUG_PRINT("AccurateRip: drive list not refreshed (%s); using the old "
                "copy\n", fetchError.String());
  }

  std::vector<uint8> data((size_t)size);
  if (cached.ReadAt(0, data.data(), data.size()) != (ssize_t)data.size()) {
    error = "The saved AccurateRip drive list could not be read";
    return B_IO_ERROR;
  }
  list.Write(data.data(), data.size());
  return B_OK;
}

/** The standard freedb disc ID, for discs without a CD:cddbid attribute. */
uint32 CddbId(const std::vector<CddaTracks::TocTrack> &tracks, uint32 leadOut) {
  auto digitSum = [](uint32 n) {
    uint32 s = 0;
    for (; n > 0; n /= 10)
      s += n % 10;
    return s;
  };
  uint32 n = 0;
  for (const CddaTracks::TocTrack &t : tracks)
    n += digitSum((t.lba + 150) / 75);
  uint32 total = (leadOut + 150) / 75 - (tracks.front().lba + 150) / 75;
  return ((n % 255) << 24) | (total << 8) | (uint32)tracks.size();
}

} // namespace

BString AccurateRip::DiscId::Url() const {
  BString hex1;
  hex1.SetToFormat("%08" B_PRIx32, id1);
  BString url;
  url.SetToFormat("http://www.accuraterip.com/accuraterip/%c/%c/%c/"
                  "dBAR-%03" B_PRId32 "-%08" B_PRIx32 "-%08" B_PRIx32
                  "-%08" B_PRIx32 ".bin",
                  hex1[7], hex1[6], hex1[5], audioTracks, id1, id2, cddb);
  return url;
}

status_t AccurateRip::ReadDiscId(const BString &cdPath, DiscId &out) {
  out = DiscId();
  CddaTracks::Toc toc;
  status_t status = CddaTracks::ReadToc(cdPath, toc);
  if (status != B_OK)
    return status;
  const std::vector<CddaTracks::TocTrack> &tracks = toc.tracks;
  const uint32 leadOut = toc.leadOut;

  for (const CddaTracks::TocTrack &t : tracks) {
    if (!t.audio)
      continue;
    out.id1 += t.lba;
    out.id2 += (t.lba > 0 ? t.lba : 1) * t.number;
    out.audioTracks++;
  }
  out.id1 += leadOut;
  out.id2 += leadOut * (uint32)(out.audioTracks + 1);

  BNode root(cdPath.String());
  uint32 cddb = 0;
  if (root.ReadAttr("CD:cddbid", B_UINT32_TYPE, 0, &cddb, sizeof(cddb)) ==
      (ssize_t)sizeof(cddb))
    out.cddb = cddb;
  else
    out.cddb = CddbId(tracks, leadOut);

  return out.IsValid() ? B_OK : B_BAD_DATA;
}

int32 AccurateRip::Database::Match(size_t track, uint32 crc) const {
  int32 best = -1;
  for (const Response &r : responses) {
    if (track < r.crc.size() && r.crc[track] == crc &&
        (int32)r.confidence[track] > best)
      best = r.confidence[track];
  }
  return best;
}

int32 AccurateRip::Database::Match450(size_t track, uint32 crc) const {
  int32 best = -1;
  for (const Response &r : responses) {
    if (track < r.frame450.size() && r.frame450[track] != 0 &&
        r.frame450[track] == crc && (int32)r.confidence[track] > best)
      best = r.confidence[track];
  }
  return best;
}

status_t AccurateRip::Parse(const void *data, size_t length, const DiscId &id,
                            Database &out) {
  out.responses.clear();
  const uint8 *p = static_cast<const uint8 *>(data);
  size_t pos = 0;
  // Responses follow one another: a 13-byte header (track count, id1, id2,
  // cddb id) and 9 bytes per track (confidence, CRC, CRC of frame 450).
  while (pos + 13 <= length) {
    const size_t tracks = p[pos];
    const size_t next = pos + 13 + tracks * 9;
    if (next > length)
      return B_BAD_DATA;
    if (ReadLE32(p + pos + 1) != id.id1 || ReadLE32(p + pos + 5) != id.id2)
      return B_BAD_DATA;

    Response r;
    for (size_t t = 0; t < tracks; t++) {
      const uint8 *e = p + pos + 13 + t * 9;
      r.confidence.push_back(e[0]);
      r.crc.push_back(ReadLE32(e + 1));
      r.frame450.push_back(ReadLE32(e + 5));
    }
    out.responses.push_back(r);
    pos = next;
  }
  return out.IsEmpty() ? B_ENTRY_NOT_FOUND : B_OK;
}

status_t AccurateRip::Fetch(const DiscId &id, Database &out, BString &error) {
  out.responses.clear();
  if (!id.IsValid()) {
    error = "This disc has no readable table of contents";
    return B_BAD_VALUE;
  }

  BMallocIO sink;
  int32 status = 0;
  status_t result = HttpGet(id.Url(), sink, status, error);
  if (result != B_OK)
    return result;
  if (status == 404)
    return B_ENTRY_NOT_FOUND;
  if (status != 200) {
    if (status == 0)
      error = "Could not reach the AccurateRip server";
    else
      error.SetToFormat("The AccurateRip server answered HTTP %ld",
                        (long)status);
    return B_ERROR;
  }

  status_t parsed = Parse(sink.Buffer(), sink.BufferLength(), id, out);
  if (parsed == B_BAD_DATA)
    error = "The AccurateRip answer could not be read";
  return parsed;
}

void AccurateRip::Checksum::Begin(uint32 frames, bool firstTrack,
                                  bool lastTrack) {
  fPosition = 1;
  fLow = fHigh = 0;
  // whipper: "MulBy >= 2940" on the first track, "MulBy <= N - 2940" on the
  // last; frame numbers are 1-based.
  fFrom = firstTrack ? (uint32)kSkipFrames : 1;
  fTo = lastTrack ? (frames > (uint32)kSkipFrames ? frames - kSkipFrames : 0)
                  : frames;
}

void AccurateRip::Checksum::Add(const uint32 *frames, size_t count) {
  for (size_t i = 0; i < count; i++, fPosition++) {
    if (fPosition < fFrom || fPosition > fTo)
      continue;
    const uint64 product = (uint64)B_LENDIAN_TO_HOST_INT32(frames[i]) * fPosition;
    fLow += (uint32)product;
    fHigh += (uint32)(product >> 32);
  }
}

bool AccurateRip::FindOffset(const uint32 *x, uint32 n, int32 range,
                             size_t track, const Database &db, int32 &offset,
                             int32 &confidence) {
  // S(o) = sum_{k=1..n} k * f[o+k-1] and T(o) = sum f[o+k-1], with f the
  // track's frames at offset o (f[0] is x[range + o]). Then
  // S(o+1) = S(o) - T(o) + n * f[o+n] and T(o+1) = T(o) - f[o] + f[o+n].
  auto frame = [&](int64 i) { return B_LENDIAN_TO_HOST_INT32(x[i]); };

  uint32 s = 0, t = 0;
  for (uint32 k = 1; k <= n; k++) {
    s += k * frame(k - 1);
    t += frame(k - 1);
  }

  bool found = false;
  confidence = -1;
  for (int32 o = -range; o <= range; o++) {
    const int32 c = db.Match(track, s);
    // Prefer the most confident match; on a tie, the smaller offset.
    if (c > confidence ||
        (c == confidence && found && abs(o) < abs(offset))) {
      if (c >= 0) {
        confidence = c;
        offset = o;
        found = true;
      }
    }
    if (o == range)
      break;
    const uint32 in = frame((int64)(o + range) + n);
    const uint32 out = frame(o + range);
    s = s - t + n * in;
    t = t - out + in;
  }
  return found;
}

void AccurateRip::FindOffset450(const uint32 *x, int32 range, size_t track,
                                const Database &db,
                                std::map<int32, int32> &matches) {
  // The same sliding sum as FindOffset(), over one sector.
  const uint32 n = 588;
  auto frame = [&](int64 i) { return B_LENDIAN_TO_HOST_INT32(x[i]); };
  uint32 s = 0, t = 0;
  for (uint32 k = 1; k <= n; k++) {
    s += k * frame(k - 1);
    t += frame(k - 1);
  }
  matches.clear();
  for (int32 o = -range; o <= range; o++) {
    const int32 c = db.Match450(track, s);
    if (c >= 0)
      matches[o] = c;
    if (o == range)
      break;
    const uint32 in = frame((int64)(o + range) + n);
    const uint32 out = frame(o + range);
    s = s - t + n * in;
    t = t - out + in;
  }
}

BString AccurateRip::DriveName(const BString &cdPath) {
  fs_info info;
  if (fs_stat_dev(dev_for_path(cdPath.String()), &info) != B_OK)
    return BString();
  int fd = open(info.device_name, O_RDONLY);
  if (fd < 0)
    return BString();
  char name[B_FILE_NAME_LENGTH] = {0};
  const bool ok = ioctl(fd, B_GET_DEVICE_NAME, name, sizeof(name)) == 0;
  close(fd);
  BString result(ok ? name : "");
  result.Trim();
  return result;
}

bool AccurateRip::DriveNameMatches(const BString &driveName,
                                   const char *listName) {
  // The list names a drive "Vendor - Model", each part as the drive reports
  // it with its padding; the device name is "Vendor Model Revision".
  const char *dash = strstr(listName, " - ");
  BString key;
  if (dash != nullptr) {
    key = NormalizedName(listName, (size_t)(dash - listName));
    const BString model = NormalizedName(dash + 3, strlen(dash + 3));
    if (!key.IsEmpty() && !model.IsEmpty())
      key << ' ';
    key << model;
  } else {
    key = NormalizedName(listName, strlen(listName));
  }
  const BString name = NormalizedName(driveName.String(), driveName.Length());
  if (key.IsEmpty() || name.IsEmpty())
    return false;
  if (name == key)
    return true;
  // What's left after the model may only be the firmware revision.
  return name.StartsWith(key) && name[key.Length()] == ' ' &&
         name.FindFirst(' ', key.Length() + 1) < 0;
}

status_t AccurateRip::ListedDriveOffset(const BString &driveName,
                                        int32 &offset, int32 &submissions,
                                        BString &error) {
  if (driveName.IsEmpty())
    return B_ENTRY_NOT_FOUND;
  BMallocIO list;
  status_t status = LoadDriveList(list, error);
  if (status != B_OK)
    return status;

  // A drive can be listed more than once (one entry per firmware family);
  // the entry with the most submissions wins.
  const uint8 *p = static_cast<const uint8 *>(list.Buffer());
  const size_t length = list.BufferLength();
  bool found = false;
  for (size_t at = 0; at + kDriveRecord <= length; at += kDriveRecord) {
    char name[34] = {0};
    memcpy(name, p + at + 2, 33);
    if (!DriveNameMatches(driveName, name))
      continue;
    const int32 count = (int32)ReadLE32(p + at + 35);
    if (found && count <= submissions)
      continue;
    offset = (int16)(p[at] | (p[at + 1] << 8));
    submissions = count;
    found = true;
  }
  return found ? B_OK : B_ENTRY_NOT_FOUND;
}
