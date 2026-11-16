/*
 * Copyright 2005-2026, Daniel Weber.
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "AudioPlaybackEngine.h"
#include "DLNAService.h"
#include "Debug.h"
#include "GainStore.h"
#include "LocalFileHttpServer.h"
#include "Messages.h"
#include "MetadataTagIO.h"
#include "NetworkAudioStreamIO.h"

#if ENABLE_LOCAL_OUTPUT
#include "AudioOutputManager.h"
#endif

#include <Entry.h>
#include <File.h>
#include <Message.h>
#include <MidiSynthFile.h>
#include <OS.h>
#include <Path.h>
#include <Url.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdio.h>
#include <vector>

using namespace BPrivate::Network;

static BLocker sMidiHookLock("midi hook");
static AudioPlaybackEngine *sMidiHookController = nullptr;

static void EmptyMidiFileHook(int32) {}

static constexpr bigtime_t kAudioFadeDurationUs = 80000;
/** The sound player's name, which the system mixer shows. */
static const char *kMixerName = "Beton";

static int64 FadeFrameCount(const media_raw_audio_format &format) {
  if (format.frame_rate <= 0)
    return 1;
  return std::max<int64>(
      1, static_cast<int64>((format.frame_rate * kAudioFadeDurationUs) /
                            1000000.0f));
}

#if ENABLE_MIDI_PLAYBACK
struct MidiTempoEvent {
  uint32 tick;
  uint32 tempo;
};

static uint16 ReadBE16(const uint8 *data) {
  return ((uint16)data[0] << 8) | data[1];
}

static uint32 ReadBE32(const uint8 *data) {
  return ((uint32)data[0] << 24) | ((uint32)data[1] << 16) |
         ((uint32)data[2] << 8) | data[3];
}

static bool ReadMidiVar(const uint8 *data, size_t size, size_t &offset,
                        uint32 &value) {
  value = 0;
  for (int i = 0; i < 4; i++) {
    if (offset >= size)
      return false;
    uint8 byte = data[offset++];
    value = (value << 7) | (byte & 0x7f);
    if ((byte & 0x80) == 0)
      return true;
  }
  return true;
}

static bool SkipMidiData(const uint8 *data, size_t size, size_t &offset,
                         size_t count) {
  if (offset + count > size)
    return false;
  offset += count;
  return true;
}

static bool ComputeMidiDuration(const entry_ref &ref, bigtime_t &duration,
                                int32 &tickDuration) {
  duration = 0;
  tickDuration = 0;

  BFile file(&ref, B_READ_ONLY);
  if (file.InitCheck() != B_OK)
    return false;

  off_t fileSize = 0;
  if (file.GetSize(&fileSize) != B_OK || fileSize < 14 ||
      fileSize > 16 * 1024 * 1024) {
    return false;
  }

  std::vector<uint8> bytes((size_t)fileSize);
  if (file.Read(bytes.data(), bytes.size()) != (ssize_t)bytes.size())
    return false;

  const uint8 *data = bytes.data();
  size_t size = bytes.size();
  size_t offset = 0;
  if (memcmp(data, "MThd", 4) != 0)
    return false;
  offset += 4;

  uint32 headerSize = ReadBE32(data + offset);
  offset += 4;
  if (headerSize < 6 || offset + headerSize > size)
    return false;

  uint16 tracks = ReadBE16(data + offset + 2);
  int16 division = (int16)ReadBE16(data + offset + 4);
  offset += headerSize;

  uint32 maxTick = 0;
  std::vector<MidiTempoEvent> tempos;
  tempos.push_back({0, 500000});

  for (uint16 track = 0; track < tracks && offset + 8 <= size; track++) {
    if (memcmp(data + offset, "MTrk", 4) != 0)
      break;
    offset += 4;
    uint32 trackSize = ReadBE32(data + offset);
    offset += 4;
    if (offset + trackSize > size)
      return false;

    size_t trackEnd = offset + trackSize;
    uint32 tick = 0;
    uint8 runningStatus = 0;
    while (offset < trackEnd) {
      uint32 delta = 0;
      if (!ReadMidiVar(data, trackEnd, offset, delta))
        return false;
      tick += delta;
      maxTick = std::max(maxTick, tick);
      if (offset >= trackEnd)
        break;

      uint8 status = data[offset++];
      if (status < 0x80) {
        if (runningStatus == 0)
          return false;
        offset--;
        status = runningStatus;
      } else if (status < 0xf0) {
        runningStatus = status;
      }

      if (status == 0xff) {
        if (offset >= trackEnd)
          return false;
        uint8 metaType = data[offset++];
        uint32 length = 0;
        if (!ReadMidiVar(data, trackEnd, offset, length))
          return false;
        if (offset + length > trackEnd)
          return false;
        if (metaType == 0x51 && length == 3) {
          uint32 tempo = ((uint32)data[offset] << 16) |
                         ((uint32)data[offset + 1] << 8) | data[offset + 2];
          if (tempo > 0)
            tempos.push_back({tick, tempo});
        }
        offset += length;
      } else if (status == 0xf0 || status == 0xf7) {
        uint32 length = 0;
        if (!ReadMidiVar(data, trackEnd, offset, length) ||
            !SkipMidiData(data, trackEnd, offset, length)) {
          return false;
        }
      } else {
        uint8 type = status & 0xf0;
        size_t dataBytes = (type == 0xc0 || type == 0xd0) ? 1 : 2;
        if (!SkipMidiData(data, trackEnd, offset, dataBytes))
          return false;
      }
    }
    offset = trackEnd;
  }

  if (maxTick == 0)
    return false;
  tickDuration = (int32)std::min(maxTick, (uint32)0x7fffffff);

  if (division < 0) {
    int8 fpsByte = (int8)((division >> 8) & 0xff);
    int fps = -fpsByte;
    int subframes = division & 0xff;
    if (fps <= 0 || subframes <= 0)
      return false;
    duration = ((bigtime_t)maxTick * 1000000LL) / (fps * subframes);
    return duration > 0;
  }

  uint16 ticksPerQuarter = (uint16)division;
  if (ticksPerQuarter == 0)
    return false;

  std::sort(tempos.begin(), tempos.end(),
            [](const MidiTempoEvent &a, const MidiTempoEvent &b) {
              return a.tick < b.tick;
            });

  uint32 lastTick = 0;
  uint32 currentTempo = 500000;
  bigtime_t total = 0;
  for (const auto &event : tempos) {
    if (event.tick > maxTick)
      break;
    if (event.tick > lastTick) {
      total += ((bigtime_t)(event.tick - lastTick) * currentTempo) /
               ticksPerQuarter;
      lastTick = event.tick;
    }
    currentTempo = event.tempo;
  }
  if (maxTick > lastTick) {
    total += ((bigtime_t)(maxTick - lastTick) * currentTempo) /
             ticksPerQuarter;
  }

  duration = total;
  return duration > 0;
}
#endif

AudioPlaybackEngine::AudioPlaybackEngine() {}

AudioPlaybackEngine::~AudioPlaybackEngine() {
#if ENABLE_LOCAL_OUTPUT
  _WaitForPrime();
#endif
  Stop();
}

/**
 * @brief Sets the messenger for notifying the UI about playback events.
 *
 * @param target The messenger (usually the MainWindow).
 */
void AudioPlaybackEngine::SetTarget(BMessenger target) { fTarget = target; }

#if ENABLE_DLNA_OUTPUT
void AudioPlaybackEngine::SetRemoteOutputManagers(
    DLNAService *dlna, LocalFileHttpServer *localServer) {
  fDlnaManager = dlna;
  fLocalFileHttpServer = localServer;
}
#endif

/**
 * @brief Starts the BMessageRunner that sends periodic time updates to the UI.
 */
void AudioPlaybackEngine::_StartTimeUpdates() {
  if (fUpdateRunner == nullptr && fTarget.IsValid()) {
    fUpdateRunner =
        new BMessageRunner(fTarget, new BMessage(MSG_TIME_UPDATE), 500000);
  }
}

/**
 * @brief Stops the periodic time updates.
 */
void AudioPlaybackEngine::_StopTimeUpdates() {
  if (fUpdateRunner) {
    delete fUpdateRunner;
    fUpdateRunner = nullptr;
  }
}

/**
 * @brief Cleans up media resources (BSoundPlayer, BMediaTrack, BMediaFile).
 *
 * Ensures thread safety and proper resource deallocation order.
 */
void AudioPlaybackEngine::_CleanupMedia() {

  _StopMidi(true);

  if (fNetworkStream) {
    fNetworkStream->Stop();
    snooze(50000);
  }

  _StopPrebufferThread();

  if (fPlayer) {
    snooze(20000);
    delete fPlayer;
    fPlayer = nullptr;
  }

  bigtime_t callbackDeadline = system_time() + 500000;
  while (fInCallback.load(std::memory_order_relaxed) &&
         system_time() < callbackDeadline) {
    snooze(1000);
  }

  if (fTrack) {
    fMediaFile->ReleaseTrack(fTrack);
    fTrack = nullptr;
  }
  if (fMediaFile) {
    delete fMediaFile;
    fMediaFile = nullptr;
  }
  if (fNetworkStream) {
    delete fNetworkStream;
    fNetworkStream = nullptr;
  }
#if ENABLE_LOCAL_OUTPUT
  // Safe here: the player is deleted and we waited for fInCallback to clear
  // above, so the audio thread is no longer touching fResampler.
  fResampler.Unset();
  fLocalOutputManager.Release();
#endif
}

/**
 * @brief Sets the playback volume.
 *
 * @param percent Volume level between 0.0 and 1.0.
 */
void AudioPlaybackEngine::SetVolume(float vol) {
  if (vol < 0.0f)
    vol = 0.0f;
  if (vol > 1.0f)
    vol = 1.0f;
  fVolume = vol;
#if ENABLE_LOCAL_OUTPUT
  // Keep the direct-mode software gain in step with the volume slider.
  _RecomputeDirectGain();
#endif

#if ENABLE_DLNA_OUTPUT
  if (fDlnaManager && fDlnaManager->IsRemoteOutput()) {
    static std::atomic<bool> sIsSettingVolume(false);
    if (sIsSettingVolume.exchange(true))
      return; ///< Prevent thread explosion while dragging volume slider

    int32 percent = (int32)(vol * 100);
    struct VolData {
      DLNAService *mgr;
      int32 vol;
    };
    VolData *data = new VolData{fDlnaManager, percent};
    thread_id tid = spawn_thread(
        [](void *arg) -> int32 {
          VolData *d = (VolData *)arg;
          d->mgr->SetRendererVolume(d->vol);
          delete d;
          sIsSettingVolume = false;
          return 0;
        },
        "dlna_vol", B_NORMAL_PRIORITY, data);
    resume_thread(tid);
    return;
  }
#endif

  if (fPlayer) {
#if ENABLE_LOCAL_OUTPUT
    if (fLocalOutputTarget == OutputTarget::SystemDefault) {
      fPlayer->SetVolume(fVolume);
    }
#else
    fPlayer->SetVolume(fVolume);
#endif
  }
  if (fMidiSynth) {
    fMidiSynth->SetVolume(fVolume);
  }
}

void AudioPlaybackEngine::_BeginFadeIn() {
  fFadeOutFrames.store(0, std::memory_order_relaxed);
  fFadeInFrames.store(-1, std::memory_order_relaxed);
}

void AudioPlaybackEngine::_BeginFadeOut() {
  fFadeInFrames.store(0, std::memory_order_relaxed);
  fFadeOutFrames.store(-1, std::memory_order_relaxed);
}

void AudioPlaybackEngine::_ApplyFade(void *buffer, size_t size,
                                     const media_raw_audio_format &format) {
  const int bytesPerSample =
      format.format & media_raw_audio_format::B_AUDIO_SIZE_MASK;
  const int channelCount = format.channel_count;
  const int frameSize = bytesPerSample * channelCount;
  if (!buffer || size == 0 || bytesPerSample <= 0 || channelCount <= 0 ||
      frameSize <= 0)
    return;

  const int64 totalFrames = FadeFrameCount(format);
  const int64 frames = static_cast<int64>(size / frameSize);
  if (frames <= 0)
    return;

  int64 fadeIn = fFadeInFrames.load(std::memory_order_relaxed);
  int64 fadeOut = fFadeOutFrames.load(std::memory_order_relaxed);
  if (fadeIn < 0) {
    fadeIn = totalFrames;
    fFadeInFrames.store(fadeIn, std::memory_order_relaxed);
  }
  if (fadeOut < 0) {
    fadeOut = totalFrames;
    fFadeOutFrames.store(fadeOut, std::memory_order_relaxed);
  }

  bool needSoftwareVolume = false;
#if ENABLE_LOCAL_OUTPUT
  if (fLocalOutputTarget != OutputTarget::SystemDefault) {
    needSoftwareVolume = true;
  }
#endif

  const float replayGain = fReplayGain.load(std::memory_order_relaxed);
  const float replayGainFrom = fAppliedReplayGain;
  fAppliedReplayGain = replayGain;
  if (fadeIn <= 0 && fadeOut <= 0 && !needSoftwareVolume &&
      replayGain == 1.0f && replayGainFrom == 1.0f)
    return;

  auto gainForFrame = [&](int64 frame) {
    float gain = 1.0f;
    if (fadeIn > 0)
      gain *= static_cast<float>(totalFrames - fadeIn + frame) / totalFrames;
    if (fadeOut > 0)
      gain *= static_cast<float>(fadeOut - frame) / totalFrames;

    if (needSoftwareVolume) {
      // fDirectGain already folds in the user's volume AND the mixer-curve
      // loudness match (or just the volume when no compensation is active).
      gain *= fDirectGain.load(std::memory_order_relaxed);
    }

    if (gain < 0.0f)
      gain = 0.0f;
    if (gain > 1.0f)
      gain = 1.0f;
    // ReplayGain may lift a quiet track above unity; it was limited to the
    // track's peak when it was set, so it does not clip. A new value glides
    // in over this buffer.
    if (replayGainFrom != replayGain && frames > 0)
      return gain * (replayGainFrom + (replayGain - replayGainFrom) *
                                          (float)(frame + 1) / frames);
    return gain * replayGain;
  };

  if (format.format == media_raw_audio_format::B_AUDIO_FLOAT) {
    float *samples = static_cast<float *>(buffer);
    for (int64 frame = 0; frame < frames; ++frame) {
      float gain = gainForFrame(frame);
      for (int channel = 0; channel < channelCount; ++channel)
        samples[frame * channelCount + channel] *= gain;
    }
  } else if (bytesPerSample == 2) {
    int16 *samples = static_cast<int16 *>(buffer);
    for (int64 frame = 0; frame < frames; ++frame) {
      float gain = gainForFrame(frame);
      for (int channel = 0; channel < channelCount; ++channel) {
        int32 scaled =
            static_cast<int32>(samples[frame * channelCount + channel] * gain);
        samples[frame * channelCount + channel] =
            static_cast<int16>(
                std::max<int32>(-32768, std::min<int32>(32767, scaled)));
      }
    }
  } else if (bytesPerSample == 4) {
    int32 *samples = static_cast<int32 *>(buffer);
    for (int64 frame = 0; frame < frames; ++frame) {
      float gain = gainForFrame(frame);
      for (int channel = 0; channel < channelCount; ++channel) {
        double scaled = samples[frame * channelCount + channel] * gain;
        samples[frame * channelCount + channel] =
            static_cast<int32>(std::max<double>(
                -2147483648.0, std::min<double>(2147483647.0, scaled)));
      }
    }
  }

  if (fadeIn > 0)
    fFadeInFrames.store(std::max<int64>(0, fadeIn - frames),
                        std::memory_order_relaxed);
  if (fadeOut > 0)
    fFadeOutFrames.store(std::max<int64>(0, fadeOut - frames),
                         std::memory_order_relaxed);
}

#if ENABLE_LOCAL_OUTPUT
// Extra source frames kept buffered inside the resampler so a full device
// buffer can always be produced despite the converter's fractional-frame
// latency (avoids a perpetual 1-frame shortfall when upsampling).
static const int64 kResampleCushion = 128;

int64 AudioPlaybackEngine::_FillResampled(void *buffer, int64 wantFrames,
                                          bool &hitEof) {
  hitEof = false;
  const int srcFrameSize = fResampler.SrcFrameSize();
  const int dstFrameSize = fResampler.DstFrameSize();
  if (!fTrack || srcFrameSize <= 0 || dstFrameSize <= 0 || wantFrames <= 0)
    return 0;

  uint8 *out = (uint8 *)buffer;
  int64 outFilled = 0;

  // A resampler cannot emit output without being fed input (its filter needs
  // upcoming samples), so we ALWAYS feed source here — never a zero-input
  // drain. Feeding SourceFramesForOutput(need) plus a small cushion lets each
  // convert emit the full `need`, buffering the cushion. SourceFramesForOutput
  // subtracts what is already buffered, so the buffer stays ~cushion-sized and
  // does not grow. The guard is a hard stop so the callback can never spin.
  for (int guard = 0; outFilled < wantFrames && guard < 64; ++guard) {
    int64 need = wantFrames - outFilled;
    uint8 *dst = out + outFilled * dstFrameSize;

    if (hitEof) {
      // Flush the resampler's final tail (NULL input) after end-of-stream.
      int64 got = fResampler.Convert(nullptr, 0, dst, need);
      outFilled += got;
      if (got == 0)
        break;
      continue;
    }

    int64 srcNeed = fResampler.SourceFramesForOutput(need) + kResampleCushion;
    size_t needBytes = (size_t)srcNeed * srcFrameSize;
    if (fResampleScratch.size() < needBytes)
      fResampleScratch.resize(needBytes); // defensive; normally pre-sized

    int64 srcFrames = srcNeed;
    status_t ret = fTrack->ReadFrames(fResampleScratch.data(), &srcFrames);
    if (ret != B_OK || srcFrames <= 0) {
      int32 zeroCount = fZeroReadCount.fetch_add(1, std::memory_order_relaxed) + 1;
      if (zeroCount > 15 || ret == B_LAST_BUFFER_ERROR) {
        hitEof = true;
      }
      continue; // flush on the next pass
    }
    fZeroReadCount.store(0, std::memory_order_relaxed);
    outFilled += fResampler.Convert(fResampleScratch.data(), srcFrames, dst,
                                    need);
  }

  return outFilled;
}

// Replicate the Haiku audio mixer's non-linear dB->gain curve
// (src/add-ons/media/media-add-ons/mixer/AudioMixer.cpp dB_to_Gain). The
// mixer's gain sliders report a warped "dB" value, so a displayed -39.7 dB is
// NOT 10^(-39.7/20) of linear gain. To match the mixer's actual loudness we
// must run its displayed dB through the same curve.
static float MixerDbToGain(float db) {
  const float kDbMax = 18.0f;
  const float kDbMin = -60.0f;
  const float kExpPos = 1.4f;
  const float kExpNeg = 1.8f;
  if (db > 0.0f) {
    db = db * (powf(fabsf(kDbMax), 1.0f / kExpPos) / fabsf(kDbMax));
    db = powf(db, kExpPos);
  } else {
    db = -db;
    db = db * (powf(fabsf(kDbMin), 1.0f / kExpNeg) / fabsf(kDbMin));
    db = powf(db, kExpNeg);
    db = -db;
  }
  return powf(10.0f, db / 20.0f);
}

void AudioPlaybackEngine::_RecomputeDirectGain() {
  float v = fVolume.load(std::memory_order_relaxed);
  if (!fDirectCompActive.load(std::memory_order_relaxed)) {
    // No mixer compensation (fell back to mixer, or not yet connected):
    // apply just the user's volume, as before.
    fDirectGain.store(v, std::memory_order_relaxed);
    return;
  }
  // Replicate the mixer's total gain on our stream: its non-linear volume
  // curve applied to fVolume (BSoundPlayer::SetVolume sets the mixer input to
  // 20*log10(fVolume) displayed dB) times its master gain, both via the mixer
  // curve. This is the exact loudness the mixer path would produce.
  float volDb = v > 0.0f ? 20.0f * log10f(v) : -144.0f;
  float g = MixerDbToGain(volDb) * MixerDbToGain(fMixerMasterDb.load(std::memory_order_relaxed));
  // The mixer applies a straight x0.708 (~ -3 dB) to its output when the
  // "Attenuate mixer output by 3 dB" toggle is on; replicate it if set.
  if (fMixerAttenuate3dB.load(std::memory_order_relaxed))
    g *= 0.708f;
  if (g > 1.0f)
    g = 1.0f;
  if (g < 0.0f)
    g = 0.0f;
  fDirectGain.store(g, std::memory_order_relaxed);
}

void AudioPlaybackEngine::_UpdateDirectGain() {
  float dB = 0.0f;
  if (fLocalOutputManager.GetSystemMixerGainDB(dB) == B_OK) {
    fMixerMasterDb.store(dB, std::memory_order_relaxed);
    fMixerAttenuate3dB.store(fLocalOutputManager.GetSystemMixerAttenuate3dB(),
                             std::memory_order_relaxed);
    fDirectCompActive.store(true, std::memory_order_relaxed);
    _RecomputeDirectGain();
    DEBUG_PRINT("Direct gain: mixer master %.2f dB (curve) atten3dB=%d x fVol "
                "-> gain x%.4f\n", dB,
                (int)fMixerAttenuate3dB.load(), fDirectGain.load());
  } else {
    fDirectCompActive.store(false, std::memory_order_relaxed);
    _RecomputeDirectGain();
    DEBUG_PRINT("Could not read mixer gain; direct gain = fVolume only\n");
  }
}
#endif

status_t AudioPlaybackEngine::_StartMidiAt(int32 position) {
  if (!fMidiSynth)
    return B_ERROR;

  fSuppressMidiHook.store(false, std::memory_order_relaxed);
  fMidiSynth->SetFileHook(&AudioPlaybackEngine::_MidiFileHook, 0);
  fMidiSynth->SetVolume(fVolume);
  fMidiSynth->Position(position);
  status_t st = fMidiSynth->Start();
  if (st == B_OK) {
    bigtime_t basePos = 0;
    int32 duration = fMidiTickDuration.load(std::memory_order_relaxed);
    if (duration <= 0)
      duration = fMidiSynth->Duration();
    if (duration > 0 && fDuration > 0)
      basePos = ((bigtime_t)position * fDuration) / duration;
    fMidiPosition.store(position, std::memory_order_relaxed);
    fMidiBasePos.store(basePos, std::memory_order_relaxed);
    fMidiStartTime.store(system_time(), std::memory_order_relaxed);
    fIsMidiPlaying.store(true, std::memory_order_relaxed);
    fMidiRunning.store(true, std::memory_order_relaxed);
  }
  return st;
}

void AudioPlaybackEngine::_StopMidi(bool unload) {
  BAutolock midiLock(&fMidiLock);
  if (!fMidiSynth)
    return;

  fMidiSeekSerial.fetch_add(1, std::memory_order_relaxed);
  fSuppressMidiHook.store(true, std::memory_order_relaxed);
  fMidiPosition.store(fMidiSynth->Seek(), std::memory_order_relaxed);
  _SilenceMidi();
  fMidiSynth->SetFileHook(&EmptyMidiFileHook, 0);
  if (fMidiRunning.exchange(false, std::memory_order_relaxed)) {
    fMidiSynth->Stop();
    snooze(20000);
  }

  if (!unload)
    return;

  fMidiSynth->UnloadFile();
  delete fMidiSynth;
  fMidiSynth = nullptr;
  {
    BAutolock hookLock(&sMidiHookLock);
    if (sMidiHookController == this)
      sMidiHookController = nullptr;
  }
  fIsMidiPlaying.store(false, std::memory_order_relaxed);
  fMidiRunning.store(false, std::memory_order_relaxed);
  fSuppressMidiHook.store(false, std::memory_order_relaxed);
  fMidiPosition.store(0, std::memory_order_relaxed);
  fMidiTickDuration.store(0, std::memory_order_relaxed);
  fMidiBasePos.store(0, std::memory_order_relaxed);
  fMidiStartTime.store(0, std::memory_order_relaxed);
}

void AudioPlaybackEngine::_SilenceMidi() {
  if (!fMidiSynth)
    return;

  fMidiSynth->SetVolume(0.0);
}

void AudioPlaybackEngine::_NotifyPlayFailed(const char *path) {
  if (!fTarget.IsValid())
    return;
  BMessage m(MSG_PLAY_FAILED);
  m.AddString("path", path);
  fTarget.SendMessage(&m);
}

void AudioPlaybackEngine::_StartPrebufferThread(int frameSize) {
  _StopPrebufferThread();
  fPrebufferFrameSize = frameSize > 0 ? frameSize : 4;
  fReaderEof.store(false, std::memory_order_relaxed);
  fPendingSeek.store(-1, std::memory_order_relaxed);
  fFlushRequested.store(false, std::memory_order_relaxed);
  fPrebufferRing.Init(524288);
  fPrebufferRunning.store(true, std::memory_order_relaxed);
  fPrebufferThread = spawn_thread(_PrebufferThreadEntry, "cd_prebuffer", B_NORMAL_PRIORITY, this);
  if (fPrebufferThread >= 0) {
    resume_thread(fPrebufferThread);
    fUsePrebuffer.store(true, std::memory_order_relaxed);
    DEBUG_PRINT("Background prebuffer thread started\n");
  }
}

void AudioPlaybackEngine::_StopPrebufferThread() {
  fUsePrebuffer.store(false, std::memory_order_relaxed);
  if (fPrebufferRunning.exchange(false)) {
    if (fPrebufferThread >= 0) {
      status_t exitVal;
      wait_for_thread(fPrebufferThread, &exitVal);
      fPrebufferThread = -1;
    }
  }
  fPrebufferRing.Reset();
}

int32 AudioPlaybackEngine::_PrebufferThreadEntry(void *cookie) {
  AudioPlaybackEngine *self = static_cast<AudioPlaybackEngine *>(cookie);
  if (self)
    self->_PrebufferThreadFunc();
  return 0;
}

void AudioPlaybackEngine::_PrebufferThreadFunc() {
  const int frameSize = fPrebufferFrameSize;
  const int64 kFramesPerRead = 4096;
  const size_t kBytesPerRead = (size_t)kFramesPerRead * frameSize;
  std::vector<uint8> scratch(kBytesPerRead);
  while (fPrebufferRunning.load(std::memory_order_relaxed)) {
    // Seeks are done here, by the only thread that reads fTrack. Everything
    // already in the ring was decoded before the seek; mark it for the
    // callback to drop, or it would play out first (up to 3 s of it).
    bigtime_t seekTo = fPendingSeek.exchange(-1, std::memory_order_relaxed);
    if (seekTo >= 0 && fTrack) {
      if (fTrack->SeekToTime(&seekTo, B_MEDIA_SEEK_CLOSEST_BACKWARD) == B_OK) {
        while (fFlushSpin.test_and_set(std::memory_order_acquire))
          ;
        fFlushTo = fPrebufferRing.WritePos();
        fFlushPos = seekTo;
        fFlushRequested.store(true, std::memory_order_relaxed);
        fFlushSpin.clear(std::memory_order_release);
        fReaderEof.store(false, std::memory_order_relaxed);
      }
      continue;
    }

    if (!fReaderEof.load(std::memory_order_relaxed) &&
        fPrebufferRing.AvailableWrite() >= kBytesPerRead) {
      int64 framesToRead = kFramesPerRead;
      status_t ret = fTrack ? fTrack->ReadFrames(scratch.data(), &framesToRead) : B_ERROR;
      if (ret == B_OK && framesToRead > 0) {
        size_t bytesRead = (size_t)framesToRead * frameSize;
        fPrebufferRing.Write(scratch.data(), bytesRead);
      } else {
        fReaderEof.store(true, std::memory_order_relaxed);
      }
    } else {
      snooze(10000);
    }
  }
}

/**
 * @brief Plays the track at the specified index in the queue.
 *
 * Stops current playback, initializes BMediaFile and BMediaTrack,
 * sets up audio format, and starts the BSoundPlayer.
 *
 * @param trackIndex Index of the track in fQueue to play.
 */
void AudioPlaybackEngine::Play(size_t trackIndex) {
  DEBUG_PRINT("Play(%zu) called\n", trackIndex);

  fCurrentBitrate.store(0);
  fCurrentSampleRate.store(0);
  fCurrentChannels.store(0);
  fZeroReadCount.store(0);
#if ENABLE_LOCAL_OUTPUT
  // Default to no compensation; the direct-connect path enables it on success.
  fDirectCompActive.store(false, std::memory_order_relaxed);
  _RecomputeDirectGain();
#endif

  Stop(true);
  snooze(10000);

  if (trackIndex >= fQueue.size()) {
    DEBUG_PRINT("index %zu out of range (queue size %zu)\n", trackIndex,
                fQueue.size());
    return;
  }

  fCurrentIdx = trackIndex;
  const char *path = fQueue[trackIndex].c_str();
  DEBUG_PRINT("opening: %s\n", path);
  _UpdateReplayGain(trackIndex);

#if ENABLE_DLNA_OUTPUT || ENABLE_MIDI_PLAYBACK
  BString lowerPath = path;
  lowerPath.ToLower();
  const bool isMidiFile =
      lowerPath.EndsWith(".mid") || lowerPath.EndsWith(".midi");
#endif

#if ENABLE_DLNA_OUTPUT
  if (fDlnaManager && fDlnaManager->IsRemoteOutput() && !isMidiFile) {
    BString targetUrl;
    if (fLocalFileHttpServer) {
      fLocalFileHttpServer->ServeFile(BString(path), targetUrl);
    }

    entry_ref ref;
    if (get_ref_for_path(path, &ref) == B_OK) {
      BMediaFile tempFile(&ref);
      if (tempFile.InitCheck() == B_OK) {
        BMediaTrack *tempTrack = tempFile.TrackAt(0);
        if (tempTrack) {
          fDuration = tempTrack->Duration();
          tempFile.ReleaseTrack(tempTrack);
        }
      }
    }

    struct PlayData {
      DLNAService *mgr;
      BString url;
    };
    PlayData *data = new PlayData{fDlnaManager, targetUrl};
    thread_id tid = spawn_thread(
        [](void *arg) -> int32 {
          PlayData *d = (PlayData *)arg;
          d->mgr->SetAVTransportURI(d->url, "");
          d->mgr->RendererPlay();
          delete d;
          return 0;
        },
        "dlna_play", B_NORMAL_PRIORITY, data);
    resume_thread(tid);

    fPlaying = true;
    fIsRemotePlaying = true;
    fIsStreaming = true; ///< Prevents local duration parsing

    if (fTarget.IsValid()) {
      BMessage m(MSG_NOW_PLAYING);
      m.AddInt32("index", fCurrentIdx);
      m.AddBool("streaming", true);
      m.AddString("path", targetUrl);
      fTarget.SendMessage(&m);
    }

    _StartTimeUpdates();
    return;
  }
#endif

  entry_ref ref;
  status_t st = get_ref_for_path(path, &ref);
  if (st != B_OK) {
    DEBUG_PRINT("get_ref_for_path failed: %s (%ld)\n", strerror(st),
                (long)st);
    _NotifyPlayFailed(path);
    return;
  }

#if ENABLE_MIDI_PLAYBACK
  if (isMidiFile) {
    fMidiSynth = new BMidiSynthFile();
    st = fMidiSynth->LoadFile(&ref);
    if (st != B_OK) {
      DEBUG_PRINT("BMidiSynthFile::LoadFile failed: %s (%ld)\n",
                  strerror(st), (long)st);
      _CleanupMedia();
      _NotifyPlayFailed(path);
      return;
    }

    {
      BAutolock hookLock(&sMidiHookLock);
      sMidiHookController = this;
    }
    fIsMidiPlaying = true;
    fPlaying = true;
    fPaused = false;
    fAtEnd = false;
    fCurrentPos = 0;
    int32 midiTickDuration = 0;
    if (ComputeMidiDuration(ref, fDuration, midiTickDuration))
      fMidiTickDuration.store(midiTickDuration, std::memory_order_relaxed);
    else
      fDuration = (bigtime_t)fMidiSynth->Duration() * 1000LL;

    st = _StartMidiAt(0);
    if (st != B_OK) {
      DEBUG_PRINT("BMidiSynthFile::Start failed: %s (%ld)\n",
                  strerror(st), (long)st);
      _CleanupMedia();
      _NotifyPlayFailed(path);
      return;
    }

    if (fTarget.IsValid()) {
      BMessage m(MSG_NOW_PLAYING);
      m.AddInt32("index", (int32)trackIndex);
      m.AddString("path", path);
      fTarget.SendMessage(&m);
    }

    _StartTimeUpdates();
    return;
  }
#endif

  fMediaFile = new BMediaFile(&ref);
  st = fMediaFile->InitCheck();
  if (st != B_OK) {
    DEBUG_PRINT("BMediaFile::InitCheck failed: %s (%ld)\n",
                strerror(st), (long)st);
    _CleanupMedia();
    _NotifyPlayFailed(path);
    return;
  }

  fTrack = fMediaFile->TrackAt(0);
  if (!fTrack) {
    DEBUG_PRINT("TrackAt(0) returned nullptr\n");
    _CleanupMedia();
    _NotifyPlayFailed(path);
    return;
  }

  fDuration = fTrack->Duration();
  DEBUG_PRINT("duration: %lld us (%.2f s)\n", (long long)fDuration,
              fDuration / 1e6);

  media_format mf{};
  st = fTrack->DecodedFormat(&mf);
    
    media_format encFmt;
    fTrack->EncodedFormat(&encFmt);
    if (encFmt.type == B_MEDIA_ENCODED_AUDIO) {
        fCurrentBitrate.store(encFmt.u.encoded_audio.bit_rate / 1000);
    } else {
        fCurrentBitrate.store(0);
    }
    
    if (st == B_OK && mf.type == B_MEDIA_RAW_AUDIO) {
        fCurrentSampleRate.store(mf.u.raw_audio.frame_rate);
        fCurrentChannels.store(mf.u.raw_audio.channel_count);
    } else {
        fCurrentSampleRate.store(0);
        fCurrentChannels.store(0);
    }
    
    if (st != B_OK) {
    DEBUG_PRINT("DecodedFormat failed: %s (%ld)\n", strerror(st),
                (long)st);
    _CleanupMedia();
    _NotifyPlayFailed(path);
    return;
  }

  const media_raw_audio_format &raf = mf.u.raw_audio;
  DEBUG_PRINT("decoded: rate=%.0f Hz, channels=%d, format=0x%lx, "
              "byte_order=%s, buffer=%ld\n",
              raf.frame_rate, (int)raf.channel_count, (unsigned long)raf.format,
              raf.byte_order == B_MEDIA_BIG_ENDIAN ? "BE" : "LE",
              (long)raf.buffer_size);

#if ENABLE_LOCAL_OUTPUT
  if (fLocalOutputTarget != OutputTarget::SystemDefault) {
    // Read the mixer's master gain and set the loudness compensation BEFORE
    // Acquire() disconnects the mixer — a disconnected mixer reports a default
    // gain, which would defeat the match.
    _UpdateDirectGain();

    media_node node;
    media_input input;
    status_t acquireStatus = fLocalOutputManager.Acquire(fLocalOutputBus, fLocalOutputTarget, fLocalConflictPolicy, fLocalFallbackDevice, node, input);
    if (acquireStatus == B_OK) {
      // Leave the format wildcarded so the connection negotiates to the
      // device/bus's supported format instead of forcing the file's format.
      // A rate-fixed device (e.g. 48 kHz only) would otherwise fail to
      // connect on a 44.1 kHz file. We then resample the decoded stream to
      // the negotiated format ourselves (see the resampler setup below).
      media_multi_audio_format format = media_multi_audio_format::wildcard;

      fPlayer = new BSoundPlayer(node, &format, kMixerName, &input, &_PlayBuffer, NULL, this);

      if (fPlayer && fPlayer->InitCheck() == B_OK) {
        // The decoder emits the file's NATIVE format (raf, from DecodedFormat
        // above) and does not resample the sample rate — only the system
        // mixer does. A physical device node may run at a fixed, different
        // rate (e.g. 48/96 kHz), so feeding native-rate frames straight to it
        // played too fast/high-pitched ("chipmunks"). Convert native ->
        // negotiated device format ourselves via libswresample.
        media_raw_audio_format devFmt = fPlayer->Format();
        status_t rs = fResampler.Init(raf, devFmt);
        if (rs != B_OK) {
          DEBUG_PRINT("Resampler init failed (%s), falling back to mixer\n",
                      strerror(rs));
          delete fPlayer;
          fPlayer = nullptr;
          fLocalOutputManager.Release();
          fPlayer = new BSoundPlayer(&raf, kMixerName, &_PlayBuffer, NULL, this);
        } else {
          // Pre-size the source scratch for one device buffer so the audio
          // callback never allocates: worst-case source frames per callback.
          if (fResampler.IsActive() && fResampler.SrcFrameSize() > 0) {
            int64 dstFrames = fResampler.DstFrameSize() > 0
                ? (int64)(devFmt.buffer_size / fResampler.DstFrameSize()) : 0;
            int64 srcFrames = fResampler.SourceFramesForOutput(dstFrames)
                + kResampleCushion + 64;
            fResampleScratch.resize((size_t)srcFrames * fResampler.SrcFrameSize());
          }
          DEBUG_PRINT("Direct output: decoded %.0f Hz/%dch -> device %.0f Hz/%dch"
                      " buffer_size=%ld (%s)\n", raf.frame_rate,
                      (int)raf.channel_count, devFmt.frame_rate,
                      (int)devFmt.channel_count, (long)devFmt.buffer_size,
                      fResampler.IsActive() ? "resampling" : "passthrough");
          // Display the track's native rate/channels, not the device's.
          fCurrentSampleRate.store((int32)raf.frame_rate);
          fCurrentChannels.store((int32)raf.channel_count);
        }
      } else {
        // Direct connection failed (e.g. device rejected the format).
        // Release the device (restores the mixer) and fall back to default.
        DEBUG_PRINT("Direct BSoundPlayer InitCheck failed, falling back to default mixer\n");
        fDirectCompActive.store(false, std::memory_order_relaxed);
        _RecomputeDirectGain();
        delete fPlayer;
        fPlayer = nullptr;
        fLocalOutputManager.Release();
        if (fTarget.IsValid()) {
          BMessage errMsg(MSG_STATUS_UPDATE);
          errMsg.AddString("text", "Device unavailable, falling back to system default");
          fTarget.SendMessage(&errMsg);
        }
        fPlayer = new BSoundPlayer(&raf, kMixerName, &_PlayBuffer, NULL, this);
      }
    } else {
      DEBUG_PRINT("Acquire device failed, falling back to default mixer\n");
      fDirectCompActive.store(false, std::memory_order_relaxed);
      _RecomputeDirectGain();
      if (fTarget.IsValid()) {
        BMessage errMsg(MSG_STATUS_UPDATE);
        errMsg.AddString("text", "Device in use, falling back to system default");
        fTarget.SendMessage(&errMsg);
      }
      fPlayer = new BSoundPlayer(&raf, kMixerName, &_PlayBuffer, NULL, this);
    }
  } else {
    fPlayer = new BSoundPlayer(&raf, kMixerName, &_PlayBuffer, NULL, this);
  }
#else
  fPlayer = new BSoundPlayer(&raf, kMixerName, &_PlayBuffer, NULL, this);
#endif

  if (!fPlayer || fPlayer->InitCheck() != B_OK) {
    DEBUG_PRINT("BSoundPlayer new/init failed\n");
    _CleanupMedia();
    _NotifyPlayFailed(path);
    return;
  }

#if ENABLE_LOCAL_OUTPUT
  if (fLocalOutputTarget == OutputTarget::SystemDefault) {
    fPlayer->SetVolume(fVolume);
  }
#else
  fPlayer->SetVolume(fVolume);
#endif
  _BeginFadeIn();

  fPlayer->Start();
  fPlayer->SetHasData(true);

  // The prebuffer thread reads native frames from fTrack in the background
  // and is the callback's only reader on that path. When the direct-output
  // resampler is active, _PlayBuffer's _FillResampled() branch reads fTrack
  // itself instead (see below) — starting the prebuffer thread too would
  // have two threads calling BMediaTrack::ReadFrames() concurrently.
#if ENABLE_LOCAL_OUTPUT
  if (!fResampler.IsActive()) {
    const int nativeFrameSize =
        (raf.format & media_raw_audio_format::B_AUDIO_SIZE_MASK) *
        raf.channel_count;
    _StartPrebufferThread(nativeFrameSize);
  }
#else
  {
    const int nativeFrameSize =
        (raf.format & media_raw_audio_format::B_AUDIO_SIZE_MASK) *
        raf.channel_count;
    _StartPrebufferThread(nativeFrameSize);
  }
#endif

  if (fTarget.IsValid()) {
    BMessage m(MSG_NOW_PLAYING);
    m.AddInt32("index", (int32)trackIndex);
    m.AddString("path", path);
    fTarget.SendMessage(&m);
  }

  fAtEnd.store(false, std::memory_order_relaxed);
  fPlaying.store(true, std::memory_order_relaxed);
  fPaused.store(false, std::memory_order_relaxed);
  fCurrentPos = 0;

  _StartTimeUpdates();

  DEBUG_PRINT("started OK\n");
}

/**
 * @brief Plays an internet radio stream from a URL.
 *
 * Uses NetworkAudioStreamIO's FFmpeg backend for HTTP/HTTPS, ICY and HLS.
 * Disables seeking and sets duration to 0 (live stream).
 *
 * @param url The stream URL to play.
 */
void AudioPlaybackEngine::SetReplayGainMode(int32 mode) {
  if (mode < kReplayGainOff || mode > kReplayGainAuto)
    mode = kReplayGainAuto;
  fReplayGainMode.store(mode);
  // Takes effect from the next track; re-evaluate the current one now.
  if (fCurrentIdx < fQueue.size() && !fIsStreaming.load())
    _UpdateReplayGain(fCurrentIdx);
}

void AudioPlaybackEngine::RefreshReplayGain(const BString &path) {
  const size_t index = fCurrentIdx;
  if (fIsStreaming.load() || index >= fQueue.size() ||
      path != fQueue[index].c_str())
    return;
  _UpdateReplayGain(index);
}

bool AudioPlaybackEngine::_IsPlayedInAlbumOrder(size_t index) {
  TagData current;
  if (!MetadataTagIO::ReadTags(BPath(fQueue[index].c_str()), current) ||
      current.album.IsEmpty() || current.track == 0)
    return false;

  auto adjacent = [&](size_t other, int32 expectedTrack) {
    if (other >= fQueue.size() || expectedTrack <= 0)
      return false;
    TagData t;
    return MetadataTagIO::ReadTags(BPath(fQueue[other].c_str()), t) &&
           t.album == current.album && (int32)t.track == expectedTrack;
  };
  return (index > 0 && adjacent(index - 1, (int32)current.track - 1)) ||
         adjacent(index + 1, (int32)current.track + 1);
}

void AudioPlaybackEngine::_UpdateReplayGain(size_t index) {
  float linear = 1.0f;
  float db = 0.0f;
  const int32 mode = fReplayGainMode.load();

  ReplayGainInfo rg;
  bool found = false;
  if (mode != kReplayGainOff && index < fQueue.size()) {
    const char *path = fQueue[index].c_str();
    found = MetadataTagIO::ReadReplayGain(BPath(path), rg);
    // A gain Beton measured but kept in the library, not in the file.
    ReplayGainInfo stored;
    if (!rg.HasTrack() && GainStore::Get(BString(path), stored) &&
        stored.HasTrack()) {
      rg.trackGain = stored.trackGain;
      rg.trackPeak = stored.trackPeak;
      found = true;
    }
  }
  if (found) {
    bool album = false;
    if (mode == kReplayGainAlbum)
      album = rg.HasAlbum();
    else if (mode == kReplayGainAuto)
      album = rg.HasAlbum() && !fShuffleHint.load() &&
              _IsPlayedInAlbumOrder(index);

    const float gain = album ? rg.albumGain : rg.trackGain;
    const float peak = album ? rg.albumPeak : rg.trackPeak;
    if (!std::isnan(gain)) {
      db = gain;
      linear = powf(10.0f, gain / 20.0f);
      // Never push the loudest sample past full scale.
      if (!std::isnan(peak) && peak > 0.0f && linear * peak > 1.0f) {
        linear = 1.0f / peak;
        db = 20.0f * log10f(linear);
      }
    }
    DEBUG_PRINT("ReplayGain: %s gain %+.2f dB (tag %+.2f, peak %.4f) -> x%.4f\n",
                album ? "album" : "track", db, gain, peak, linear);
  }
  fReplayGain.store(linear);
  fReplayGainDb.store(db);
}

void AudioPlaybackEngine::PlayUrl(const BUrl &url, const char *title,
                                      int32 durationSeconds,
                                      BUrlContext *context) {
  fCurrentBitrate.store(0);
  fCurrentSampleRate.store(0);
  fCurrentChannels.store(0);
#if ENABLE_LOCAL_OUTPUT
  fDirectCompActive.store(false, std::memory_order_relaxed);
  _RecomputeDirectGain();
#endif
  fCurrentUrl = url;
  fCurrentTitle = title ? title : "";
  // Streams carry no ReplayGain tags.
  fReplayGain.store(1.0f);
  fReplayGainDb.store(0.0f);

  BAutolock lock(fPlayLock);
  DEBUG_PRINT("PlayUrl(%s, duration=%ld) called\n",
              url.UrlString().String(), (long)durationSeconds);

  _StopLocked(true);
  snooze(10000);

#if ENABLE_DLNA_OUTPUT
  BString targetUrl = url.UrlString();
  BString lowerUrl = targetUrl;
  lowerUrl.ToLower();
  const bool isMidiUrl =
      lowerUrl.EndsWith(".mid") || lowerUrl.EndsWith(".midi");

  if (fDlnaManager && fDlnaManager->IsRemoteOutput() && !isMidiUrl) {
    if (!targetUrl.StartsWith("http")) {
      if (fLocalFileHttpServer) {
        fLocalFileHttpServer->ServeFile(targetUrl, targetUrl);
      }
    }
    struct PlayData {
      DLNAService *mgr;
      BString url;
      BString title;
    };
    PlayData *data = new PlayData{fDlnaManager, targetUrl, title ? title : ""};
    thread_id tid = spawn_thread(
        [](void *arg) -> int32 {
          PlayData *d = (PlayData *)arg;
          d->mgr->SetAVTransportURI(d->url, d->title);
          d->mgr->RendererPlay();
          delete d;
          return 0;
        },
        "dlna_play", B_NORMAL_PRIORITY, data);
    resume_thread(tid);

    fPlaying = true;
    fIsRemotePlaying = true;
    fIsStreaming = true; ///< Prevents local duration parsing
    fDuration = (bigtime_t)durationSeconds * 1000000;

    if (fTarget.IsValid()) {
      BMessage m(MSG_NOW_PLAYING);
      m.AddInt32("index", fCurrentIdx);
      m.AddBool("streaming", true);
      m.AddString("path", targetUrl);
      if (title && strlen(title) > 0)
        m.AddString("title", title);
      fTarget.SendMessage(&m);
    }

    _StartTimeUpdates();
    return;
  }

  fIsRemotePlaying = false;
#endif

  fNetworkStream =
      new NetworkAudioStreamIO(fTarget, context ? context : &fUrlContext);

#if ENABLE_LOCAL_OUTPUT
  if (fLocalOutputTarget != OutputTarget::SystemDefault) {
    media_format fmt;
    fmt.type = B_MEDIA_RAW_AUDIO;
    BMediaRoster* roster = BMediaRoster::Roster();
    if (roster && roster->GetFormatFor(fLocalOutputBus.input, &fmt) == B_OK) {
      if (fmt.type == B_MEDIA_RAW_AUDIO) {
        fNetworkStream->SetTargetFormat(fmt.u.raw_audio.frame_rate, fmt.u.raw_audio.channel_count);
      }
    }
  }
#endif

  NetworkAudioStreamIO::Mode mode = NetworkAudioStreamIO::MODE_ICY;
  if (url.UrlString().IEndsWith(".m3u8"))
    mode = NetworkAudioStreamIO::MODE_HLS;
  else if (context != nullptr)
    mode = NetworkAudioStreamIO::MODE_DLNA;

  status_t st = fNetworkStream->Open(url.UrlString(), mode);
  if (st != B_OK) {
    DEBUG_PRINT("NetworkAudioStreamIO::Open failed\n");
    if (mode == NetworkAudioStreamIO::MODE_DLNA && fTarget.IsValid()) {
      BMessage m(MSG_DLNA_RESOURCE_UNAVAILABLE);
      m.AddString("path", url.UrlString().String());
      if (title && strlen(title) > 0)
        m.AddString("title", title);
      fTarget.SendMessage(&m);
    }
    _CleanupMedia();
    return;
  }

  fIsStreaming.store(durationSeconds == 0, std::memory_order_relaxed);
  fDuration = (bigtime_t)durationSeconds * 1000000;

  media_raw_audio_format raf{};
  st = fNetworkStream->WaitForFormat(&raf, 30000000);
  if (st != B_OK) {
    DEBUG_PRINT("WaitForFormat failed/timed out\n");
    if (mode == NetworkAudioStreamIO::MODE_DLNA && fTarget.IsValid()) {
      BMessage m(MSG_DLNA_RESOURCE_UNAVAILABLE);
      m.AddString("path", url.UrlString().String());
      if (title && strlen(title) > 0)
        m.AddString("title", title);
      fTarget.SendMessage(&m);
    }
    _CleanupMedia();
    fIsStreaming.store(false, std::memory_order_relaxed);
    return;
  }
  fCurrentSampleRate.store(raf.frame_rate);
  fCurrentChannels.store(raf.channel_count);
  DEBUG_PRINT("FFmpeg stream format: rate=%.0f ch=%ld\n",
              raf.frame_rate, (long)raf.channel_count);

  if (mode == NetworkAudioStreamIO::MODE_HLS) {
    size_t prebufferBytes =
        (size_t)raf.frame_rate * raf.channel_count * sizeof(float) * 2;
    status_t prebufferStatus = fNetworkStream->WaitForData(prebufferBytes,
                                                           15000000);
    DEBUG_PRINT("HLS PCM prebuffer: requested=%zu status=%ld\n",
                prebufferBytes, (long)prebufferStatus);
  }

#if ENABLE_LOCAL_OUTPUT
  if (fLocalOutputTarget != OutputTarget::SystemDefault) {
    // Read mixer gain before Acquire() disconnects the mixer (see Play()).
    _UpdateDirectGain();

    media_node node;
    media_input input;
    status_t acquireStatus = fLocalOutputManager.Acquire(fLocalOutputBus, fLocalOutputTarget, fLocalConflictPolicy, fLocalFallbackDevice, node, input);
    if (acquireStatus == B_OK) {
      // Wildcard the format so the connection negotiates to the device's
      // supported format rather than forcing the stream's format (see Play()).
      media_multi_audio_format format = media_multi_audio_format::wildcard;

      fPlayer = new BSoundPlayer(node, &format, kMixerName, &input, &_PlayBuffer, NULL, this);

      if (fPlayer && fPlayer->InitCheck() == B_OK) {
        // NOTE: the stream's resample target was already set before Open()
        // (via GetFormatFor(bus.input) above), so WaitForFormat() returned the
        // bus-format raf and the wildcard connection negotiates to match it.
        // We only refresh the display fields here.
        media_raw_audio_format pf = fPlayer->Format();
        fCurrentSampleRate.store(pf.frame_rate);
        fCurrentChannels.store(pf.channel_count);
      } else {
        // Direct connection failed; release the device and fall back.
        DEBUG_PRINT("Direct BSoundPlayer InitCheck failed, falling back to default mixer\n");
        fDirectCompActive.store(false, std::memory_order_relaxed);
        _RecomputeDirectGain();
        delete fPlayer;
        fPlayer = nullptr;
        fLocalOutputManager.Release();
        if (fTarget.IsValid()) {
          BMessage errMsg(MSG_STATUS_UPDATE);
          errMsg.AddString("text", "Device unavailable, falling back to system default");
          fTarget.SendMessage(&errMsg);
        }
        fPlayer = new BSoundPlayer(&raf, kMixerName, &_PlayBuffer, NULL, this);
      }
    } else {
      DEBUG_PRINT("Acquire device failed, falling back to default mixer\n");
      fDirectCompActive.store(false, std::memory_order_relaxed);
      _RecomputeDirectGain();
      if (fTarget.IsValid()) {
        BMessage errMsg(MSG_STATUS_UPDATE);
        errMsg.AddString("text", "Device in use, falling back to system default");
        fTarget.SendMessage(&errMsg);
      }
      fPlayer = new BSoundPlayer(&raf, kMixerName, &_PlayBuffer, NULL, this);
    }
  } else {
    fPlayer = new BSoundPlayer(&raf, kMixerName, &_PlayBuffer, NULL, this);
  }
#else
  fPlayer = new BSoundPlayer(&raf, kMixerName, &_PlayBuffer, NULL, this);
#endif

  if (!fPlayer || fPlayer->InitCheck() != B_OK) {
    _CleanupMedia();
    fIsStreaming.store(false, std::memory_order_relaxed);
    return;
  }

#if ENABLE_LOCAL_OUTPUT
  if (fLocalOutputTarget == OutputTarget::SystemDefault) {
    fPlayer->SetVolume(fVolume);
  }
#else
  fPlayer->SetVolume(fVolume);
#endif
  _BeginFadeIn();
  fPlayer->Start();
  fPlayer->SetHasData(true);

  if (fTarget.IsValid()) {
    BMessage m(MSG_NOW_PLAYING);
    m.AddInt32("index", 0);
    m.AddString("path", url.UrlString().String());
    if (title && strlen(title) > 0)
      m.AddString("title", title);
    m.AddBool("streaming", true);
    fTarget.SendMessage(&m);
  }

  fAtEnd.store(false, std::memory_order_relaxed);
  fPlaying.store(true, std::memory_order_relaxed);
  fPaused.store(false, std::memory_order_relaxed);
  fCurrentPos = 0;

  _StartTimeUpdates();

  DEBUG_PRINT("stream started via NetworkAudioStreamIO\n");
}

/**
 * @brief Pauses playback.
 */
void AudioPlaybackEngine::Pause() {
#if ENABLE_DLNA_OUTPUT
  if (fIsRemotePlaying && fDlnaManager) {
    DLNAService *mgr = fDlnaManager;
    thread_id tid = spawn_thread(
        [](void *arg) -> int32 {
          ((DLNAService *)arg)->RendererPause();
          return 0;
        },
        "dlna_pause", B_NORMAL_PRIORITY, mgr);
    resume_thread(tid);
    fPaused.store(true, std::memory_order_relaxed);
    fPlaying.store(false, std::memory_order_relaxed);
    return;
  }
#endif

  if (fPlayer && fPlaying.load(std::memory_order_relaxed)) {
    _BeginFadeOut();
    snooze(kAudioFadeDurationUs);
    fPlayer->Stop();
    fFadeOutFrames.store(0, std::memory_order_relaxed);
    fPaused.store(true, std::memory_order_relaxed);
    fPlaying.store(false, std::memory_order_relaxed);
  }
  if (fMidiSynth && fPlaying.load(std::memory_order_relaxed)) {
    BAutolock midiLock(&fMidiLock);
    if (!fMidiSynth || !fPlaying.load(std::memory_order_relaxed))
      return;
    bigtime_t pos = CurrentPosition();
    fCurrentPos = pos;
    fMidiPosition.store(fMidiSynth->Seek(), std::memory_order_relaxed);
    fMidiBasePos.store(pos, std::memory_order_relaxed);
    fMidiStartTime.store(0, std::memory_order_relaxed);
    _SilenceMidi();
    fMidiSynth->Pause();
    fPaused.store(true, std::memory_order_relaxed);
    fPlaying.store(false, std::memory_order_relaxed);
  }
}

/**
 * @brief Resumes paused playback.
 */
void AudioPlaybackEngine::Resume() {
#if ENABLE_DLNA_OUTPUT
  if (fIsRemotePlaying && fDlnaManager) {
    DLNAService *mgr = fDlnaManager;
    thread_id tid = spawn_thread(
        [](void *arg) -> int32 {
          ((DLNAService *)arg)->RendererPlay();
          return 0;
        },
        "dlna_resume", B_NORMAL_PRIORITY, mgr);
    resume_thread(tid);
    fPaused.store(false, std::memory_order_relaxed);
    fPlaying.store(true, std::memory_order_relaxed);
    return;
  }
#endif

  if (fPlayer && fPaused.load(std::memory_order_relaxed)) {
    _BeginFadeIn();
    fPlayer->Start();
    fPlayer->SetHasData(true);
    fPaused.store(false, std::memory_order_relaxed);
    fPlaying.store(true, std::memory_order_relaxed);
  }
  if (fMidiSynth && fPaused.load(std::memory_order_relaxed)) {
    BAutolock midiLock(&fMidiLock);
    if (!fMidiSynth || !fPaused.load(std::memory_order_relaxed))
      return;
    fMidiSynth->SetVolume(fVolume);
    fMidiBasePos.store(fCurrentPos.load(std::memory_order_relaxed),
                       std::memory_order_relaxed);
    fMidiStartTime.store(system_time(), std::memory_order_relaxed);
    fMidiSynth->Resume();
    fPaused.store(false, std::memory_order_relaxed);
    fPlaying.store(true, std::memory_order_relaxed);
  }
}

/**
 * @brief Stops playback completely and resets state.
 */
void AudioPlaybackEngine::Stop(bool switching) {
  BAutolock lock(fPlayLock);
  _StopLocked(switching);
}

void AudioPlaybackEngine::_StopLocked(bool switching) {
  DEBUG_PRINT("Stop(switching=%s) called\n",
              switching ? "true" : "false");

#if ENABLE_DLNA_OUTPUT
  if (fIsRemotePlaying && fDlnaManager && !switching) {
    DLNAService *mgr = fDlnaManager;
    thread_id tid = spawn_thread(
        [](void *arg) -> int32 {
          ((DLNAService *)arg)->RendererStop();
          return 0;
        },
        "dlna_stop", B_NORMAL_PRIORITY, mgr);
    resume_thread(tid);
    fIsRemotePlaying = false;
  }
#endif

  if (fPlayer &&
      (fPlaying.load(std::memory_order_relaxed) ||
       fPaused.load(std::memory_order_relaxed)) &&
      !fAtEnd.load(std::memory_order_relaxed)) {
    _BeginFadeOut();
    snooze(kAudioFadeDurationUs);
  }

  _StopTimeUpdates();
  fAtEnd = true;
  fPlaying.store(false, std::memory_order_relaxed);
  fPaused.store(false, std::memory_order_relaxed);
  fIsStreaming.store(false, std::memory_order_relaxed);
  fFadeInFrames.store(0, std::memory_order_relaxed);
  fFadeOutFrames.store(0, std::memory_order_relaxed);

  if (fNetworkStream) {
      DEBUG_PRINT("stopping NetworkAudioStreamIO to unblock audio callback...\n");
      fNetworkStream->Stop();
  }

  if (fPlayer) {
    DEBUG_PRINT("stopping BSoundPlayer...\n");
    fPlayer->SetHasData(false);
    fPlayer->Stop();

    snooze(20000);
  }
  _CleanupMedia();

  fCurrentPos = 0;
  fDuration = 0;
  fCurrentIdx = 0;

  DEBUG_PRINT("Stop() finished\n");
}

/**
 * @brief Plays the next track in the queue, if available.
 */
void AudioPlaybackEngine::PlayNext() {
  if (!fQueue.empty()) {
    if (fCurrentIdx + 1 < fQueue.size()) {
      fCurrentIdx++;
      Play(fCurrentIdx);
    } else {
      Stop();
    }
  }
}

/**
 * @brief Plays the previous track in the queue, if available.
 */
void AudioPlaybackEngine::PlayPrev() {
  if (!fQueue.empty()) {
    if (fCurrentIdx > 0) {
      fCurrentIdx--;
      Play(fCurrentIdx);
    } else {
      Stop();
    }
  }
}

/**
 * @brief Seeks to a specific position in the current track.
 *
 * @param pos Position in microseconds.
 */
void AudioPlaybackEngine::SeekTo(bigtime_t pos) {
#if ENABLE_DLNA_OUTPUT
  if (fDlnaManager &&
      (fIsRemotePlaying.load(std::memory_order_relaxed) ||
       fDlnaManager->IsRemoteOutput())) {
    static std::atomic<bool> sIsSeeking(false);
    static std::atomic<bigtime_t> sPendingSeek(-1);
    if (sIsSeeking.exchange(true)) {
      sPendingSeek.store(pos, std::memory_order_relaxed);
      return;
    }

    struct SeekData {
      DLNAService *mgr;
      bigtime_t pos;
    };
    SeekData *data = new SeekData{fDlnaManager, pos};
    thread_id tid = spawn_thread(
        [](void *arg) -> int32 {
          SeekData *d = (SeekData *)arg;
          for (;;) {
            status_t err = d->mgr->RendererSeek(d->pos);
            DEBUG_PRINT("DLNA seek to %lld returned %ld\n",
                        (long long)d->pos, (long)err);

            bigtime_t pending =
                sPendingSeek.exchange(-1, std::memory_order_relaxed);
            if (pending < 0 || pending == d->pos)
              break;
            d->pos = pending;
          }
          delete d;
          sIsSeeking = false;
          return 0;
        },
        "dlna_seek", B_NORMAL_PRIORITY, data);
    if (tid >= 0) {
      resume_thread(tid);
    } else {
      delete data;
      sPendingSeek.store(-1, std::memory_order_relaxed);
      sIsSeeking = false;
    }
    return;
  }
#endif

  if (fMidiSynth && fIsMidiPlaying.load(std::memory_order_relaxed)) {
    return;
  }

  if (fNetworkStream &&
      fNetworkStream->GetMode() == NetworkAudioStreamIO::MODE_DLNA) {
    status_t ret = fNetworkStream->SeekToTime(pos);
    DEBUG_PRINT("DLNA local stream seek to %lld returned %ld\n",
                (long long)pos, (long)ret);
    if (ret == B_OK)
      fCurrentPos = pos;
    return;
  }

  // Serialize against teardown. _CleanupMedia() releases fTrack (destroying the
  // decoder) and only then clears the pointer, all under fPlayLock. Without
  // holding it here, a seek can pass the null-check below, get preempted while
  // ReleaseTrack() destroys the decoder, then call SeekToTime() on freed memory
  // — AVCodecDecoder::SeekedTo() re-frees its stale fChunkBuffer and the heap
  // aborts with a double free. Seeking to the end of a track makes this likely,
  // since hitting the end starts teardown just as the seek is handled.
  BAutolock lock(fPlayLock);

  if (!fTrack || fIsStreaming.load(std::memory_order_relaxed) || fIsMidiPlaying)
    return;

  // The prebuffer thread is reading fTrack; hand the seek to it. The position
  // is corrected to the frame actually reached when the callback picks it up.
  if (fUsePrebuffer.load(std::memory_order_relaxed)) {
    fPendingSeek.store(pos, std::memory_order_relaxed);
    fCurrentPos = pos;
    return;
  }

  bigtime_t newTime = pos;
  status_t ret = fTrack->SeekToTime(&newTime, B_MEDIA_SEEK_CLOSEST_BACKWARD);
  if (ret == B_OK) {
    fCurrentPos = newTime;
  }
}

bool AudioPlaybackEngine::IsPlaying() const {
  return fPlaying.load(std::memory_order_relaxed) &&
         !fPaused.load(std::memory_order_relaxed);
}

/**
 * @brief Shuts down the controller, stopping playback and cleaning up.
 */
void AudioPlaybackEngine::Shutdown() {
  BAutolock lock(fPlayLock);
  fShuttingDown = true;
  fAtEnd = true;
#if ENABLE_LOCAL_OUTPUT
  _WaitForPrime();
#endif
  _StopTimeUpdates();

  if (fPlayer) {
    fPlayer->SetHasData(false);
    fPlayer->Stop();
  }

  _CleanupMedia();
  fTarget = BMessenger();
  fPlaying.store(false, std::memory_order_relaxed);
  fPaused.store(false, std::memory_order_relaxed);
}

bool AudioPlaybackEngine::IsPaused() const {
  return fPaused.load(std::memory_order_relaxed);
}

int32 AudioPlaybackEngine::CurrentIndex() const { return fCurrentIdx; }

void AudioPlaybackEngine::SetQueue(const std::vector<std::string> &queue) {
  fQueue = queue;
  fCurrentIdx = 0;
}

bigtime_t AudioPlaybackEngine::CurrentPosition() const {
#if ENABLE_DLNA_OUTPUT
  if (fDlnaManager && fDlnaManager->IsRemoteOutput())
    return fDlnaManager->GetCurrentPosition();
#endif
  if (fMidiSynth && fIsMidiPlaying.load(std::memory_order_relaxed)) {
    bigtime_t pos = fCurrentPos.load(std::memory_order_relaxed);
    if (!fPaused.load(std::memory_order_relaxed) &&
        fPlaying.load(std::memory_order_relaxed)) {
      bigtime_t start = fMidiStartTime.load(std::memory_order_relaxed);
      if (start > 0)
        pos = fMidiBasePos.load(std::memory_order_relaxed) +
              (system_time() - start);
    }
    if (fDuration > 0 && pos > fDuration)
      pos = fDuration;
    return pos;
  }
  return fCurrentPos;
}

void AudioPlaybackEngine::_MidiFileHook(int32) {
  AudioPlaybackEngine *controller = nullptr;
  {
    BAutolock hookLock(&sMidiHookLock);
    controller = sMidiHookController;
  }

  if (!controller ||
      controller->fShuttingDown.load(std::memory_order_relaxed) ||
      controller->fAtEnd.load(std::memory_order_relaxed) ||
      controller->fSuppressMidiHook.load(std::memory_order_relaxed) ||
      !controller->fIsMidiPlaying.load(std::memory_order_relaxed)) {
    return;
  }

  bool expected = false;
  if (!controller->fAtEnd.compare_exchange_strong(expected, true))
    return;

  controller->fPlaying.store(false, std::memory_order_relaxed);
  controller->fPaused.store(false, std::memory_order_relaxed);
  controller->fMidiRunning.store(false, std::memory_order_relaxed);

  if (controller->fTarget.IsValid()) {
    BMessage m(MSG_TRACK_ENDED);
    controller->fTarget.SendMessage(&m);
  }
}

bigtime_t AudioPlaybackEngine::Duration() const {
#if ENABLE_DLNA_OUTPUT
  if (fDlnaManager && fDlnaManager->IsRemoteOutput()) {
    bigtime_t dlnaDur = fDlnaManager->GetCurrentDuration();
    if (dlnaDur > 0)
      return dlnaDur;
  }
#endif
  return fDuration;
}

/**
 * @brief Static audio buffer callback for BSoundPlayer.
 *
 * Reads decoded frames from BMediaTrack and fills the audio buffer.
 * Handles end-of-track detection and notification.
 */
void AudioPlaybackEngine::_PlayBuffer(
    void *cookie, void *buffer, size_t size,
    const media_raw_audio_format &format) {
  auto *self = static_cast<AudioPlaybackEngine *>(cookie);
  if (!self) {
    memset(buffer, 0, size);
    return;
  }

  self->fInCallback.store(true, std::memory_order_relaxed);

  if (self->fShuttingDown.load(std::memory_order_relaxed) ||
      self->fAtEnd.load(std::memory_order_relaxed)) {
    memset(buffer, 0, size);
    self->fInCallback.store(false, std::memory_order_relaxed);
    return;
  }

  if (self->fNetworkStream) {
    ssize_t read = self->fNetworkStream->ReadPcm(buffer, size);
    if (read < 0) {
      memset(buffer, 0, size);
      if (self->fIsStreaming.load(std::memory_order_relaxed)) {
        /// Live stream (HLS/ICY): only signal end-of-track when the
        /// FFmpeg decode loop has truly finished. During HLS retries
        /// the loop is still running, so we just output silence.
        bool isFinished = !self->fNetworkStream->IsRequestRunning();
        DEBUG_PRINT("ReadPcm returned error (streaming), "
                    "isFinished=%d\n", (int)isFinished);
        if (isFinished) {
          bool expected = false;
          if (self->fAtEnd.compare_exchange_strong(expected, true) &&
              self->fTarget.IsValid()) {
            BMessage m(MSG_TRACK_ENDED);
            self->fTarget.SendMessage(&m);
          }
        }
      } else {
        /// Finite-length stream (DLNA file, etc.): end immediately
        bool expected = false;
        if (self->fAtEnd.compare_exchange_strong(expected, true) &&
            self->fTarget.IsValid()) {
          BMessage m(MSG_TRACK_ENDED);
          self->fTarget.SendMessage(&m);
        }
      }
    } else if (read > 0) {
      if ((size_t)read < size)
        memset((uint8 *)buffer + read, 0, size - read);
      self->_ApplyFade(buffer, (size_t)read, format);
      const int bytesPerSample =
          format.format & media_raw_audio_format::B_AUDIO_SIZE_MASK;
      const int frameSize = bytesPerSample * format.channel_count;
      if (frameSize > 0) {
        int64 frames = (int64)read / frameSize;
        self->fCurrentPos +=
            (bigtime_t)((frames * 1000000LL) / (int)format.frame_rate);
      }
    }
    self->fInCallback.store(false, std::memory_order_relaxed);
    return;
  }

  if (self->fTrack == nullptr) {
    memset(buffer, 0, size);
    self->fInCallback.store(false, std::memory_order_relaxed);
    return;
  }

  const int bytesPerSample =
      format.format & media_raw_audio_format::B_AUDIO_SIZE_MASK;
  const int frameSize = bytesPerSample * format.channel_count;
  int64 frames = frameSize > 0 ? (int64)(size / frameSize) : 0;

#if ENABLE_LOCAL_OUTPUT
  // Direct output to a device whose rate/channels differ from the decoded
  // file: pull native frames and convert them to the device format. When the
  // formats match, fResampler is inactive and we fall through to the plain
  // ReadFrames path below.
  if (self->fResampler.IsActive()) {
    bool hitEof = false;
    int64 filled = self->_FillResampled(buffer, frames, hitEof);
    size_t produced = (size_t)filled * frameSize;
    if (filled > 0) {
      self->fCurrentPos +=
          (bigtime_t)((filled * 1000000LL) / (int)format.frame_rate);
      self->_ApplyFade(buffer, produced, format);
    }
    if (produced < size)
      memset((uint8 *)buffer + produced, 0, size - produced);
    // Only end the track on a real end-of-stream from the decoder. A short
    // fill without EOF is just converter latency and is padded with silence.
    if (hitEof) {
      bool expected = false;
      if (!self->fShuttingDown.load(std::memory_order_relaxed) &&
          !self->fStopping.load(std::memory_order_relaxed) &&
          self->fAtEnd.compare_exchange_strong(expected, true) &&
          self->fTarget.IsValid()) {
        BMessage m(MSG_TRACK_ENDED);
        self->fTarget.SendMessage(&m);
      }
    }
    self->fInCallback.store(false, std::memory_order_relaxed);
    return;
  }
#endif

  if (self->fUsePrebuffer.load(std::memory_order_relaxed)) {
    if (self->fFlushRequested.load(std::memory_order_relaxed)) {
      while (self->fFlushSpin.test_and_set(std::memory_order_acquire))
        ;
      self->fPrebufferRing.DiscardTo(self->fFlushTo);
      self->fCurrentPos = self->fFlushPos;
      self->fFlushRequested.store(false, std::memory_order_relaxed);
      self->fFlushSpin.clear(std::memory_order_release);
    }
    size_t readBytes = self->fPrebufferRing.Read((uint8 *)buffer, size);
    if (readBytes > 0) {
      self->fZeroReadCount.store(0, std::memory_order_relaxed);
      if (frameSize > 0) {
        int64 readFrames = (int64)readBytes / frameSize;
        self->fCurrentPos +=
            (bigtime_t)((readFrames * 1000000LL) / (int)format.frame_rate);
      }
      self->_ApplyFade(buffer, readBytes, format);
      if (readBytes < size)
        memset((uint8 *)buffer + readBytes, 0, size - readBytes);
    } else if (self->fReaderEof.load(std::memory_order_relaxed)) {
      memset(buffer, 0, size);
      bool expected = false;
      if (!self->fShuttingDown.load(std::memory_order_relaxed) &&
          !self->fStopping.load(std::memory_order_relaxed) &&
          self->fAtEnd.compare_exchange_strong(expected, true)) {
        if (self->fTarget.IsValid()) {
          BMessage m(MSG_TRACK_ENDED);
          self->fTarget.SendMessage(&m);
        }
      }
    } else {
      memset(buffer, 0, size);
    }
    self->fInCallback.store(false, std::memory_order_relaxed);
    return;
  }

  status_t ret = B_ERROR;
  if (self->fTrack && frames > 0)
    ret = self->fTrack->ReadFrames(buffer, &frames);

  static int sCbCount = 0;
  if (++sCbCount % 50 == 0 || ret != B_OK || frames <= 0) {
    DEBUG_PRINT("_PlayBuffer cb=%d ret=%s (%d) frames=%lld size=%zu vol=%.2f\n",
                sCbCount, strerror(ret), (int)ret, (long long)frames, size,
                self->fVolume.load(std::memory_order_relaxed));
  }

  if (ret == B_OK && frames > 0) {
    self->fZeroReadCount.store(0, std::memory_order_relaxed);
    self->fCurrentPos +=
        (bigtime_t)((frames * 1000000LL) / (int)format.frame_rate);
    size_t produced = (size_t)frames * frameSize;
    self->_ApplyFade(buffer, produced, format);
    if (produced < size)
      memset((uint8 *)buffer + produced, 0, size - produced);
  } else if (self->fIsStreaming.load(std::memory_order_relaxed)) {
    /// If the network request is finished, this is a real EOF
    bool isFinished =
        (self->fNetworkStream && !self->fNetworkStream->IsRequestRunning());
    bool expected = false;

    if (isFinished && self->fAtEnd.compare_exchange_strong(expected, true)) {
      if (self->fTarget.IsValid()) {
        BMessage m(MSG_TRACK_ENDED);
        self->fTarget.SendMessage(&m);
      }
    }

    memset(buffer, 0, size);
    if (!isFinished)
      snooze(10000); ///< Prevent tight loop if network is just slow
  } else {
    /// End of stream or error (local files only)
    memset(buffer, 0, size);
    int32 zeroCount = self->fZeroReadCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (zeroCount > 15 || ret == B_LAST_BUFFER_ERROR) {
      bool expected = false;
      if (!self->fShuttingDown.load(std::memory_order_relaxed) &&
          !self->fStopping.load(std::memory_order_relaxed) &&
          self->fAtEnd.compare_exchange_strong(expected, true)) {

        if (self->fTarget.IsValid()) {
          BMessage m(MSG_TRACK_ENDED);
          self->fTarget.SendMessage(&m);
        }
      }
    }
  }

  self->fInCallback.store(false, std::memory_order_relaxed);
}

#if ENABLE_LOCAL_OUTPUT
namespace {
struct PrimeData {
  AudioPlaybackEngine *self;
  OutputBusInfo bus;
};
} // namespace

int32 AudioPlaybackEngine::_PrimeThreadEntry(void *arg) {
  PrimeData *d = static_cast<PrimeData *>(arg);
  d->self->fLocalOutputManager.Prime(d->bus);
  delete d;
  return 0;
}

void AudioPlaybackEngine::_WaitForPrime() {
  thread_id t = fPrimeThread.exchange(-1, std::memory_order_relaxed);
  if (t >= 0) {
    status_t ret;
    wait_for_thread(t, &ret);
  }
}

void AudioPlaybackEngine::_SpawnPrime() {
  // One prime at a time; join any prior one so fLocalOutputBus (captured
  // below) is stable for its lifetime and it never races teardown.
  _WaitForPrime();
  PrimeData *d = new PrimeData{this, fLocalOutputBus};
  thread_id t =
      spawn_thread(_PrimeThreadEntry, "beton_prime", B_NORMAL_PRIORITY, d);
  if (t >= 0) {
    fPrimeThread.store(t, std::memory_order_relaxed);
    resume_thread(t);
  } else {
    delete d;
  }
}

void AudioPlaybackEngine::SetOutputDevice(OutputTarget target,
                                         const OutputBusInfo& bus,
                                         MixerConflictPolicy policy,
                                         const BString& fallbackDeviceName) {
  BAutolock lock(fPlayLock);

  if (fLocalOutputTarget == target &&
      fLocalConflictPolicy == policy &&
      fLocalFallbackDevice == fallbackDeviceName &&
      (target == OutputTarget::SystemDefault ||
       (fLocalOutputBus.deviceName == bus.deviceName &&
        fLocalOutputBus.busName == bus.busName))) {
    return;
  }

  DEBUG_PRINT("SetOutputDevice called, switching output...\n");

  bool wasPlaying = IsPlaying();
  bool wasPaused = IsPaused();
  bigtime_t currentPos = fCurrentPos.load();
  bool isStream = IsStreaming();
  BUrl savedUrl = fCurrentUrl;
  BString savedTitle = fCurrentTitle;
  size_t savedIdx = fCurrentIdx.load();

  // 1. Stop current playback to release player and output manager node resources
  _StopLocked(true);

  // 2. Apply new parameters
  fLocalOutputTarget = target;
  fLocalOutputBus = bus;
  fLocalConflictPolicy = policy;
  fLocalFallbackDevice = fallbackDeviceName;

  // 3. Resume if we were playing or paused
  if (wasPlaying || wasPaused) {
    if (isStream) {
      PlayUrl(savedUrl, savedTitle);
      if (wasPaused) {
        Pause();
      }
    } else {
      Play(savedIdx);
      if (currentPos > 0) {
        SeekTo(currentPos);
      }
      if (wasPaused) {
        Pause();
      }
    }
  } else if (target != OutputTarget::SystemDefault) {
    // Nothing playing (startup restore, or the user picked a device while
    // stopped): warm the device in the background so the first Play() connects
    // to a running node instead of paying the cold-start settle on the play
    // path. Skipped when resuming above, since that Play() warms it anyway.
    _SpawnPrime();
  }
}
#endif
