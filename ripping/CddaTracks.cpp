/*
 * Copyright 2026, David Beckman, indispensablelabs@gmail.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "CddaTracks.h"

#include <ctype.h>
#include <Directory.h>
#include <Entry.h>
#include <Node.h>
#include <Path.h>
#include <Volume.h>
#include <fs_info.h>

#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * The disc-level artist. cdda puts per-track artists on the files but names
 * the volume "<disc artist> - <album>", which is the only place a
 * compilation's "Various Artists" survives.
 */
static BString DiscArtist(const BString &cdPath, const BString &album) {
  BNode root(cdPath.String());
  char buf[256] = {0};
  if (root.InitCheck() == B_OK &&
      root.ReadAttr("Audio:Artist", B_STRING_TYPE, 0, buf, sizeof(buf) - 1) > 0) {
    BString artist(buf);
    artist.Trim();
    if (!artist.IsEmpty())
      return artist;
  }

  if (album.IsEmpty())
    return BString();

  BVolume volume(dev_for_path(cdPath.String()));
  char name[B_FILE_NAME_LENGTH] = {0};
  if (volume.InitCheck() != B_OK || volume.GetName(name) != B_OK)
    return BString();

  // The name is "<artist> - <title>", but not always with the title exactly
  // as the tracks carry it: Haiku's cddb_lookup turns '/' into ' ', and
  // Gothic Box disc 2 (hrev60368) came up as "... Disc 2 of3" while its
  // tracks say "Disc 2 of 3". So try each " - " split and compare the rest
  // with the album ignoring spaces, punctuation and case.
  auto normalize = [](const BString &text) {
    BString out;
    for (int32 i = 0; i < text.Length(); i++) {
      const uint8 c = (uint8)text[i];
      if (c >= 0x80 || isalnum(c))
        out << (char)tolower(c);
    }
    return out;
  };
  const BString wanted = normalize(album);
  if (wanted.IsEmpty())
    return BString();
  const BString volumeName(name);
  for (int32 at = volumeName.FindFirst(" - "); at > 0;
       at = volumeName.FindFirst(" - ", at + 1)) {
    BString rest;
    volumeName.CopyInto(rest, at + 3, volumeName.Length() - at - 3);
    if (normalize(rest) != wanted)
      continue;
    BString artist;
    volumeName.CopyInto(artist, 0, at);
    artist.Trim();
    return artist;
  }
  return BString();
}

std::vector<MediaItem> CddaTracks::Read(const BString &cdPath) {
  std::vector<MediaItem> cdTracks;
  if (!cdPath.IsEmpty()) {
    BDirectory dir(cdPath.String());
    if (dir.InitCheck() == B_OK) {
      BEntry entry;
      dir.Rewind();
      int32 fallbackTrackNum = 1;
      while (dir.GetNextEntry(&entry, true) == B_OK) {
        if (!entry.IsFile())
          continue;

        BPath path;
        if (entry.GetPath(&path) != B_OK)
          continue;

        BString pathStr(path.Path());
        BString lower(pathStr);
        lower.ToLower();
        if (!lower.EndsWith(".wav") && !lower.EndsWith(".aiff") && lower.IFindFirst("track") < 0)
          continue;

        MediaItem item;
        item.path = pathStr;
        item.base = cdPath;

        BNode node(pathStr.String());
        if (node.InitCheck() == B_OK) {
          char buf[512];

          memset(buf, 0, sizeof(buf));
          if (node.ReadAttr("Media:Title", B_STRING_TYPE, 0, buf, sizeof(buf) - 1) > 0)
            item.title = buf;
          else if (node.ReadAttr("Audio:Title", B_STRING_TYPE, 0, buf, sizeof(buf) - 1) > 0)
            item.title = buf;
          else
            item.title = path.Leaf();

          memset(buf, 0, sizeof(buf));
          if (node.ReadAttr("Audio:Artist", B_STRING_TYPE, 0, buf, sizeof(buf) - 1) > 0)
            item.artist = buf;
          else if (node.ReadAttr("Media:Artist", B_STRING_TYPE, 0, buf, sizeof(buf) - 1) > 0)
            item.artist = buf;

          memset(buf, 0, sizeof(buf));
          if (node.ReadAttr("Audio:Album", B_STRING_TYPE, 0, buf, sizeof(buf) - 1) > 0)
            item.album = buf;

          memset(buf, 0, sizeof(buf));
          if (node.ReadAttr("Media:Genre", B_STRING_TYPE, 0, buf, sizeof(buf) - 1) > 0)
            item.genre = buf;
          else if (node.ReadAttr("Audio:Genre", B_STRING_TYPE, 0, buf, sizeof(buf) - 1) > 0)
            item.genre = buf;

          int32 intVal = 0;
          if (node.ReadAttr("Audio:Track", B_INT32_TYPE, 0, &intVal, sizeof(intVal)) > 0) {
            item.track = intVal;
          } else {
            memset(buf, 0, sizeof(buf));
            if (node.ReadAttr("Audio:Track", B_STRING_TYPE, 0, buf, sizeof(buf) - 1) > 0) {
              item.track = atoi(buf);
            } else {
              int t = 0;
              if (sscanf(path.Leaf(), "%d", &t) == 1 || sscanf(path.Leaf(), "Track %d", &t) == 1) {
                item.track = t;
              } else {
                item.track = fallbackTrackNum;
              }
            }
          }

          if (node.ReadAttr("Media:Year", B_INT32_TYPE, 0, &intVal, sizeof(intVal)) > 0) {
            item.year = intVal;
          }

          int64 len64 = 0;
          if (node.ReadAttr("Media:Length", B_INT64_TYPE, 0, &len64, sizeof(len64)) > 0) {
            item.duration = (int32)(len64 / 1000000LL);
          } else if (node.ReadAttr("Media:Length", B_INT32_TYPE, 0, &intVal, sizeof(intVal)) > 0) {
            item.duration = (intVal > 100000) ? (intVal / 1000000) : intVal;
          }

          off_t fileSize = 0;
          if (entry.GetSize(&fileSize) == B_OK) {
            item.size = fileSize;
            if (item.duration == 0 && fileSize > 44) {
              item.duration = (int32)((fileSize - 44) / 176400);
            }
          }
        } else {
          item.title = path.Leaf();
          item.track = fallbackTrackNum;
        }

        fallbackTrackNum++;
        cdTracks.push_back(item);
      }
    }
  }

  std::sort(cdTracks.begin(), cdTracks.end(), [](const MediaItem &a, const MediaItem &b) {
    if (a.track != b.track)
      return a.track < b.track;
    return a.path < b.path;
  });

  // Only tracks from one album get an album artist; anything else is left
  // for the tags (or {albumartist}'s fallback) to sort out.
  if (!cdTracks.empty()) {
    const BString &album = cdTracks.front().album;
    bool sameAlbum = true;
    for (const MediaItem &t : cdTracks) {
      if (t.album != album) {
        sameAlbum = false;
        break;
      }
    }
    BString discArtist = sameAlbum ? DiscArtist(cdPath, album) : BString();
    if (!discArtist.IsEmpty()) {
      for (MediaItem &t : cdTracks) {
        if (t.albumArtist.IsEmpty())
          t.albumArtist = discArtist;
      }
    }
  }

  return cdTracks;
}

status_t CddaTracks::ReadToc(const BString &cdPath, Toc &out) {
  out = Toc();
  BNode root(cdPath.String());
  if (root.InitCheck() != B_OK)
    return root.InitCheck();

  // scsi_toc_toc: 4-byte header, then 8 bytes per track with an MSF start
  // address (cdda asks the drive for MSF). Track 0xAA is the lead-out.
  uint8 toc[4 + 100 * 8];
  ssize_t size = root.ReadAttr("CD:toc", B_RAW_TYPE, 0, toc, sizeof(toc));
  if (size < 4 + 2 * 8)
    return B_ENTRY_NOT_FOUND;

  bool haveLeadOut = false;
  for (ssize_t i = 4; i + 8 <= size; i += 8) {
    const uint8 *e = toc + i;
    const uint32 lba = ((uint32)e[5] * 60 + e[6]) * 75 + e[7] - 150;
    if (e[2] == 0xAA) {
      out.leadOut = lba;
      haveLeadOut = true;
      break;
    }
    out.tracks.push_back({e[2], (e[1] & 0x04) == 0, lba});
  }
  return (out.tracks.empty() || !haveLeadOut) ? B_BAD_DATA : B_OK;
}
