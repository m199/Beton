/*
 * Copyright 2026, David Beckman, indispensablelabs@gmail.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "RipNamePattern.h"

#include <StorageDefs.h>

#include <vector>

const char *const RipNamePattern::kFields[] = {
    "{artist}", "{albumartist}", "{album}", "{title}", "{track}",
    "{tracks}", "{disc}",        "{year}",  "{genre}"};
const int32 RipNamePattern::kFieldCount =
    sizeof(RipNamePattern::kFields) / sizeof(RipNamePattern::kFields[0]);

// Room for ".flac" and the ".part" suffix used while a track is being written.
static const int32 kMaxComponentBytes = B_FILE_NAME_LENGTH - 16;

/** Lowercases and drops spaces, '_' and '-' so "{Album Artist}" matches. */
static BString NormalizeField(const BString &name) {
  BString out;
  for (int32 i = 0; i < name.Length(); i++) {
    char c = name[i];
    if (c == ' ' || c == '_' || c == '-')
      continue;
    if (c >= 'A' && c <= 'Z')
      c = c - 'A' + 'a';
    out << c;
  }
  return out;
}

static BString TwoDigits(int32 n) {
  BString s;
  s.SetToFormat("%02ld", (long)n);
  return s;
}

/** A field value as file-name text: no '/' and no control characters. */
static BString SafeValue(const BString &value) {
  BString out;
  for (int32 i = 0; i < value.Length(); i++) {
    unsigned char c = (unsigned char)value[i];
    if (c == '/')
      out << '-';
    else if (c < 0x20)
      out << ' ';
    else
      out << (char)c;
  }
  out.Trim();
  return out;
}

static bool FieldValue(const BString &field, const MediaItem &t, BString &out) {
  if (field == "artist") {
    out = t.artist.IsEmpty() ? BString("Unknown Artist") : t.artist;
  } else if (field == "albumartist") {
    if (!t.albumArtist.IsEmpty())
      out = t.albumArtist;
    else
      out = t.artist.IsEmpty() ? BString("Unknown Artist") : t.artist;
  } else if (field == "album") {
    out = t.album.IsEmpty() ? BString("Unknown Album") : t.album;
  } else if (field == "title") {
    if (!t.title.IsEmpty())
      out = t.title;
    else
      out << "Track " << TwoDigits(t.track);
  } else if (field == "track") {
    out = TwoDigits(t.track);
  } else if (field == "tracks") {
    out = t.trackTotal > 0 ? TwoDigits(t.trackTotal) : BString();
  } else if (field == "disc") {
    out.Truncate(0);
    if (t.disc > 0)
      out << t.disc;
  } else if (field == "year") {
    out.Truncate(0);
    if (t.year > 0)
      out << t.year;
  } else if (field == "genre") {
    out = t.genre;
  } else {
    return false;
  }
  out = SafeValue(out);
  return true;
}

/** Cuts @p s to at most @p maxBytes without splitting a UTF-8 character. */
static void TruncateUtf8(BString &s, int32 maxBytes) {
  if (s.Length() <= maxBytes)
    return;
  int32 cut = maxBytes;
  while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80)
    cut--;
  s.Truncate(cut);
}

/**
 * Tidies one folder or file name: trims the separators an empty field leaves
 * behind ("{year} - {album}" without a year), avoids hidden and special names.
 */
static BString CleanComponent(BString c) {
  bool changed = true;
  while (changed && !c.IsEmpty()) {
    changed = false;
    c.Trim();
    while (!c.IsEmpty() && (c[0] == '-' || c[0] == '.')) {
      c.Remove(0, 1);
      changed = true;
    }
    while (!c.IsEmpty() && c[c.Length() - 1] == '-') {
      c.Truncate(c.Length() - 1);
      changed = true;
    }
  }
  TruncateUtf8(c, kMaxComponentBytes);
  c.Trim();
  return c;
}

BString RipNamePattern::Expand(const BString &pattern, const MediaItem &track,
                               BString *unknownField) {
  if (unknownField)
    unknownField->Truncate(0);

  BString raw;
  int32 i = 0;
  const int32 len = pattern.Length();
  while (i < len) {
    char c = pattern[i];
    if (c == '{') {
      int32 close = pattern.FindFirst('}', i + 1);
      if (close > i) {
        BString name;
        pattern.CopyInto(name, i + 1, close - i - 1);
        BString value;
        if (FieldValue(NormalizeField(name), track, value)) {
          raw << value;
        } else {
          BString literal;
          pattern.CopyInto(literal, i, close - i + 1);
          if (unknownField && unknownField->IsEmpty())
            *unknownField = literal;
          raw << literal;
        }
        i = close + 1;
        continue;
      }
    }
    raw << c;
    i++;
  }

  // Split on the pattern's own slashes; field values had theirs replaced.
  BString result;
  int32 start = 0;
  while (start <= raw.Length()) {
    int32 slash = raw.FindFirst('/', start);
    if (slash < 0)
      slash = raw.Length();
    BString part;
    raw.CopyInto(part, start, slash - start);
    part = CleanComponent(part);
    if (!part.IsEmpty()) {
      if (!result.IsEmpty())
        result << '/';
      result << part;
    }
    start = slash + 1;
  }

  if (result.IsEmpty() || result.EndsWith("/")) {
    BString fallback;
    fallback << TwoDigits(track.track) << " - "
             << (track.title.IsEmpty() ? BString("Track") : SafeValue(track.title));
    result << CleanComponent(fallback);
  }
  return result;
}
