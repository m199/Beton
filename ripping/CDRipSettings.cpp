/*
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "CDRipSettings.h"

#include "MediaLibraryCache.h"

#include <File.h>
#include <FindDirectory.h>
#include <Message.h>
#include <Path.h>

#include <vector>

static const char *kSettingsFile = "Beton/cd_rip.settings";

const char *RipFormatExtension(RipFormat format) {
  switch (format) {
  case RipFormat::MP3_320:
    return "mp3";
  case RipFormat::FLAC:
    return "flac";
  case RipFormat::WAV:
    return "wav";
  }
  return "flac";
}

const char *CDRipSettings::DefaultPattern() {
  return "{albumartist}/{album}/{track} - {title}";
}

BString CDRipSettings::DefaultDirectory() {
  // Ripping into the library means the new files show up there right away.
  std::vector<BString> dirs;
  MediaLibraryCache::LoadDirectories(dirs);
  if (!dirs.empty())
    return dirs.front();

  BPath home;
  if (find_directory(B_USER_DIRECTORY, &home) != B_OK)
    return "/boot/home/music";
  home.Append("music");
  return home.Path();
}

void CDRipSettings::Load() {
  format = RipFormat::FLAC;
  directory.Truncate(0);
  pattern.Truncate(0);
  verify = true;
  addToLibrary = true;
  giveUpPercent = -1;
  fDriveOffsets.MakeEmpty();
  fOffsetCandidates.MakeEmpty();

  BPath path;
  if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK) {
    path.Append(kSettingsFile);
    BFile file(path.Path(), B_READ_ONLY);
    BMessage archive;
    if (file.InitCheck() == B_OK && archive.Unflatten(&file) == B_OK) {
      int32 value = 0;
      if (archive.FindInt32("format", &value) == B_OK &&
          value >= (int32)RipFormat::MP3_320 && value <= (int32)RipFormat::WAV)
        format = (RipFormat)value;
      archive.FindString("directory", &directory);
      archive.FindString("pattern", &pattern);
      bool b;
      if (archive.FindBool("verify", &b) == B_OK)
        verify = b;
      if (archive.FindBool("add_to_library", &b) == B_OK)
        addToLibrary = b;
      int32 percent;
      if (archive.FindInt32("give_up_percent", &percent) == B_OK &&
          percent >= -1 && percent <= 10)
        giveUpPercent = percent;
      archive.FindMessage("drive_offsets", &fDriveOffsets);
      archive.FindMessage("offset_candidates", &fOffsetCandidates);
    }
  }

  if (directory.IsEmpty())
    directory = DefaultDirectory();
  if (pattern.IsEmpty())
    pattern = DefaultPattern();
}

void CDRipSettings::Save() const {
  BPath path;
  if (find_directory(B_USER_SETTINGS_DIRECTORY, &path, true) != B_OK)
    return;
  path.Append(kSettingsFile);

  BMessage archive;
  archive.AddInt32("format", (int32)format);
  archive.AddString("directory", directory);
  archive.AddString("pattern", pattern);
  archive.AddBool("verify", verify);
  archive.AddBool("add_to_library", addToLibrary);
  archive.AddInt32("give_up_percent", giveUpPercent);
  archive.AddMessage("drive_offsets", &fDriveOffsets);
  archive.AddMessage("offset_candidates", &fOffsetCandidates);

  BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
  if (file.InitCheck() == B_OK)
    archive.Flatten(&file);
}

bool CDRipSettings::DriveOffset(const BString &drive, int32 &offset) const {
  return !drive.IsEmpty() &&
         fDriveOffsets.FindInt32(drive.String(), &offset) == B_OK;
}

void CDRipSettings::SetDriveOffset(const BString &drive, int32 offset) {
  if (drive.IsEmpty())
    return;
  fDriveOffsets.RemoveName(drive.String());
  fDriveOffsets.AddInt32(drive.String(), offset);
}

void CDRipSettings::ForgetDriveOffset(const BString &drive) {
  if (!drive.IsEmpty())
    fDriveOffsets.RemoveName(drive.String());
}

bool CDRipSettings::OffsetCandidate(const BString &drive, int32 &offset,
                                    BString &disc) const {
  BMessage entry;
  if (drive.IsEmpty() ||
      fOffsetCandidates.FindMessage(drive.String(), &entry) != B_OK)
    return false;
  // Both or nothing: Find() empties its argument when a field is missing.
  int32 o;
  BString d;
  if (entry.FindInt32("offset", &o) != B_OK ||
      entry.FindString("disc", &d) != B_OK)
    return false;
  offset = o;
  disc = d;
  return true;
}

void CDRipSettings::SetOffsetCandidate(const BString &drive, int32 offset,
                                       const BString &disc) {
  if (drive.IsEmpty())
    return;
  BMessage entry;
  entry.AddInt32("offset", offset);
  entry.AddString("disc", disc);
  fOffsetCandidates.RemoveName(drive.String());
  fOffsetCandidates.AddMessage(drive.String(), &entry);
}

void CDRipSettings::ForgetOffsetCandidate(const BString &drive) {
  if (!drive.IsEmpty())
    fOffsetCandidates.RemoveName(drive.String());
}
