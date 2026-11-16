/*
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "GainStore.h"

#include <Autolock.h>
#include <Locker.h>

#include <map>

namespace {

BLocker &Lock() {
  static BLocker sLock("gain store");
  return sLock;
}

std::map<BString, ReplayGainInfo> &Map() {
  static std::map<BString, ReplayGainInfo> sMap;
  return sMap;
}

} // namespace

void GainStore::Set(const BString &path, const ReplayGainInfo &info) {
  BAutolock guard(Lock());
  if (info.HasTrack() || info.HasAlbum())
    Map()[path] = info;
  else
    Map().erase(path);
}

void GainStore::Remove(const BString &path) {
  BAutolock guard(Lock());
  Map().erase(path);
}

bool GainStore::Get(const BString &path, ReplayGainInfo &info) {
  BAutolock guard(Lock());
  auto it = Map().find(path);
  if (it == Map().end())
    return false;
  info = it->second;
  return true;
}
