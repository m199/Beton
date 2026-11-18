/*
 * Copyright 2005-2026, Daniel Weber.
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#ifndef BETON_AUDIO_PLAYBACK_ENGINE_H
#define BETON_AUDIO_PLAYBACK_ENGINE_H

#include "Config.h"
#include "Messages.h"
#if ENABLE_LOCAL_OUTPUT
#include "AudioOutputManager.h"
#include "AudioResampler.h"
#endif

#include <Autolock.h>
#include <Locker.h>
#include <MediaFile.h>
#include <MediaTrack.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <String.h>
#include <SoundPlayer.h>
#include <Url.h>
#include <UrlContext.h>
#include <atomic>
#include <string>
#include <vector>

/**
 * @class AudioPlaybackEngine
 * @brief Manages audio playback, queue management, and playback state.
 *
 * Handles loading media files, decoding audio frames via BMediaTrack,
 * and playing them using BSoundPlayer. Manages a playback queue and
 * supports basic controls (play, pause, next, prev, seek, volume).
 *
 * Uses atomic flags to coordinate between the UI thread and the real-time
 * audio callback thread.
 */
class DLNAService;
class LocalFileHttpServer;

struct AudioRingBuffer {
  std::vector<uint8> buffer;
  size_t capacity = 0;
  std::atomic<size_t> writeHead{0};
  std::atomic<size_t> readHead{0};

  void Init(size_t sizeBytes) {
    buffer.resize(sizeBytes);
    capacity = sizeBytes;
    writeHead.store(0, std::memory_order_relaxed);
    readHead.store(0, std::memory_order_relaxed);
  }

  size_t AvailableRead() const {
    size_t w = writeHead.load(std::memory_order_acquire);
    size_t r = readHead.load(std::memory_order_relaxed);
    return (w >= r) ? (w - r) : (capacity - r + w);
  }

  size_t AvailableWrite() const {
    size_t r = readHead.load(std::memory_order_relaxed);
    size_t w = writeHead.load(std::memory_order_relaxed);
    return (r > w) ? (r - w - 1) : (capacity - w + r - 1);
  }

  size_t Write(const uint8 *src, size_t count) {
    size_t avail = AvailableWrite();
    if (count > avail)
      count = avail;
    if (count == 0)
      return 0;

    size_t w = writeHead.load(std::memory_order_relaxed);
    size_t firstPart = std::min(count, capacity - w);
    memcpy(&buffer[w], src, firstPart);
    if (count > firstPart) {
      memcpy(&buffer[0], src + firstPart, count - firstPart);
    }
    writeHead.store((w + count) % capacity, std::memory_order_release);
    return count;
  }

  size_t Read(uint8 *dst, size_t count) {
    size_t avail = AvailableRead();
    if (count > avail)
      count = avail;
    if (count == 0)
      return 0;

    size_t r = readHead.load(std::memory_order_relaxed);
    size_t firstPart = std::min(count, capacity - r);
    memcpy(dst, &buffer[r], firstPart);
    if (count > firstPart) {
      memcpy(dst + firstPart, &buffer[0], count - firstPart);
    }
    readHead.store((r + count) % capacity, std::memory_order_release);
    return count;
  }

  /** @brief Current write position. Writer side only. */
  size_t WritePos() const {
    return writeHead.load(std::memory_order_relaxed);
  }

  /** @brief Drops everything before @a pos. Reader side only. */
  void DiscardTo(size_t pos) {
    readHead.store(pos, std::memory_order_release);
  }

  void Reset() {
    writeHead.store(0, std::memory_order_relaxed);
    readHead.store(0, std::memory_order_relaxed);
  }
};

class AudioPlaybackEngine {
public:
  AudioPlaybackEngine();
  ~AudioPlaybackEngine();

#if ENABLE_DLNA_OUTPUT
  void SetRemoteOutputManagers(DLNAService *dlna, LocalFileHttpServer *localServer);
#endif

#if ENABLE_LOCAL_OUTPUT
  void SetOutputDevice(OutputTarget target, const OutputBusInfo& bus, MixerConflictPolicy policy, const BString& fallbackDeviceName);
  OutputTarget LocalOutputTarget() const { return fLocalOutputTarget; }
  OutputBusInfo LocalOutputBus() const { return fLocalOutputBus; }
  MixerConflictPolicy LocalConflictPolicy() const { return fLocalConflictPolicy; }
  BString LocalFallbackDevice() const { return fLocalFallbackDevice; }
#endif

  /**
   * @brief Sets the target messenger for playback events.
   * @param target Receiver for MSG_PLAYBACK_PREV, MSG_TRACK_ENDED, etc.
   */
  void SetTarget(BMessenger target);

  /**
   * @brief Safely shuts down the controller and playback engine.
   */
  void Shutdown();

  /** @name Playback Controls */
  ///@{
  void SetVolume(float percent);     ///< Sets volume (0.0 - 1.0).
  void Play(size_t trackIndex = 0);  ///< Plays track at specified queue index.
  void Pause();                      ///< Pauses playback.
  void Resume();                     ///< Resumes playback.
  void Stop(bool switching = false); ///< Stops playback and resets state.
  void PlayNext();                   ///< Advances to next track in queue.
  void PlayPrev();                   ///< Returns to previous track.
  void SeekTo(bigtime_t pos);        ///< Seeks to position in microseconds.
  void PlayUrl(const BUrl &url, const char *title = nullptr,
               int32 durationSeconds = 0,
               BPrivate::Network::BUrlContext *context = nullptr);
  ///@}

  /** @name State Queries */
  ///@{
  bool IsPlaying() const; ///< True if playing and not paused.
  bool IsPaused() const;  ///< True if paused.
  bool IsStreaming() const {
    return fIsStreaming.load(std::memory_order_relaxed);
  }
  int32 CurrentIndex() const; ///< Index of currently playing track.
  ///@}

  /** @name ReplayGain */
  ///@{
  /** How playback uses REPLAYGAIN_* tags. Values are stored in settings. */
  enum ReplayGainMode : int32 {
    kReplayGainOff = 0,
    kReplayGainTrack = 1, ///< Level every track to the same loudness.
    kReplayGainAlbum = 2, ///< Album gain, track gain where a file has none.
    kReplayGainAuto = 3   ///< Album gain for an album played in order.
  };
  void SetReplayGainMode(int32 mode);
  int32 ReplayGainModeValue() const { return fReplayGainMode.load(); }
  /** Auto mode uses track gain while shuffle is on. */
  void SetShuffleHint(bool shuffle) { fShuffleHint.store(shuffle); }
  /** Gain applied to the current track in dB (0 when none). */
  float CurrentReplayGainDb() const { return fReplayGainDb.load(); }
  /** Re-reads the gain of the playing track if it is @p path, as when its
   *  loudness was just measured; the level glides to the new gain. */
  void RefreshReplayGain(const BString &path);
  ///@}

  /** @name Queue Management */
  ///@{
  void SetQueue(const std::vector<std::string> &queue);
  int32 QueueSize() const { return static_cast<int32>(fQueue.size()); }
  /** Path of queue entry @p index, or empty. */
  BString QueuePathAt(int32 index) const {
    return index >= 0 && (size_t)index < fQueue.size()
               ? BString(fQueue[index].c_str())
               : BString();
  }
  ///@}

  /** @name Time Info */
  ///@{
  bigtime_t
  CurrentPosition() const;    ///< Current playback position in microseconds.
  bigtime_t Duration() const;
    int32 CurrentBitrate() const { return fCurrentBitrate.load(); }
    int32 CurrentSampleRate() const { return fCurrentSampleRate.load(); }
    int32 CurrentChannels() const { return fCurrentChannels.load(); }
 ///< Duration of current track in microseconds.
  ///@}

private:
  /**
   * @brief Audio callback function for BSoundPlayer.
   *
   * Reads decoded frames from the media track and fills the audio buffer.
   */
  static void _PlayBuffer(void *cookie, void *buffer, size_t size,
                          const media_raw_audio_format &format);

  void _StartTimeUpdates();
  void _StopTimeUpdates();
  void _CleanupMedia();
  void _StopLocked(bool switching);
  void _BeginFadeIn();
  void _BeginFadeOut();
  void _ApplyFade(void *buffer, size_t size,
                  const media_raw_audio_format &format);
  /** Works out fReplayGain for queue entry @p index from its tags. */
  void _UpdateReplayGain(size_t index);
  /** True if a queue neighbour of @p index is the adjacent album track. */
  bool _IsPlayedInAlbumOrder(size_t index);
#if ENABLE_LOCAL_OUTPUT
  /**
   * @brief Fill @p buffer with resampled local-file audio (direct output).
   *
   * Reads native-format frames from fTrack and converts them to the device
   * format via fResampler. Returns destination frames written (< wantFrames
   * signals end of track).
   */
  int64 _FillResampled(void *buffer, int64 wantFrames, bool &hitEof);
  // Capture the system mixer's master gain for direct-output loudness matching.
  // Call when a direct connection is established (before the mixer is
  // disconnected). Sets fMixerMasterDb + fDirectCompActive, then recomputes.
  void _UpdateDirectGain();
  // Recompute fDirectGain from fVolume + fMixerMasterDb, replicating the Haiku
  // mixer's non-linear volume+master curve so direct matches mixer loudness.
  // Call on any volume change and after _UpdateDirectGain.
  void _RecomputeDirectGain();
  // Warm the selected direct-output device in the background so the first
  // Play() connects to an already-running node (no cold-start settle on the
  // play path). Spawned from SetOutputDevice when nothing is playing.
  void _SpawnPrime();
  // Join any outstanding prime thread. Called before spawning a new one and
  // before teardown so the thread never outlives the engine.
  void _WaitForPrime();
  static int32 _PrimeThreadEntry(void *arg);
#endif
  status_t _StartMidiAt(int32 position);
  void _StopMidi(bool unload);
  void _SilenceMidi();
  /**
   * @brief Handles BMidiSynthFile end-of-file notifications.
   */
  static void _MidiFileHook(int32 arg);

  /** @name Media Kit Objects */
  ///@{
  BSoundPlayer *fPlayer = nullptr;
  BMediaFile *fMediaFile = nullptr;
  BMediaTrack *fTrack = nullptr;
  class BMidiSynthFile *fMidiSynth = nullptr;
  std::atomic<bool> fIsMidiPlaying{false};
  std::atomic<bool> fMidiRunning{false};
  std::atomic<bool> fSuppressMidiHook{false};
  std::atomic<int32> fMidiPosition{0};
  std::atomic<int32> fMidiTickDuration{0};
  std::atomic<int32> fMidiSeekSerial{0};
  std::atomic<bigtime_t> fMidiBasePos{0};
  std::atomic<bigtime_t> fMidiStartTime{0};
  BLocker fMidiLock;
  ///@}

  /** @name Playback Position, Volume and Index */
  ///@{
  std::atomic<bigtime_t> fCurrentPos{0};
  bigtime_t fDuration = 0;
  // Atomic: read on the real-time audio thread (software volume in _ApplyFade)
  // while written on the control thread. std::atomic<float> provides implicit
  // load/store, so existing use-sites are unchanged.
  std::atomic<float> fVolume{1.0f};
  /** ReplayGain as a linear factor, read on the audio thread in _ApplyFade.
   *  Already limited so that the track's peak does not clip. */
  std::atomic<float> fReplayGain{1.0f};
  std::atomic<float> fReplayGainDb{0.0f};
  /** The ReplayGain _ApplyFade last used; audio thread only. A change is
   *  ramped over one buffer so it doesn't click. */
  float fAppliedReplayGain = 1.0f;
  std::atomic<int32> fReplayGainMode{kReplayGainAuto};
  std::atomic<bool> fShuffleHint{false};
  std::atomic<int32> fCurrentBitrate{0};
  std::atomic<int32> fCurrentSampleRate{0};
  std::atomic<int32> fCurrentChannels{0};
  std::atomic<int64> fFadeInFrames{0};
  std::atomic<int64> fFadeOutFrames{0};

  std::atomic<size_t> fCurrentIdx{0};
  ///@}

  /** @name Queue, Thread Safety and Playback*/
  ///@{
  std::vector<std::string> fQueue;
  std::atomic<bool> fPlaying{false};
  std::atomic<bool> fPaused{false};
  std::atomic<bool> fAtEnd{false};
  std::atomic<bool> fShuttingDown{false};
  std::atomic<bool> fInCallback{false};
  std::atomic<bool> fStopping{false};
  std::atomic<int32> fZeroReadCount{0};
  std::atomic<bool> fIsStreaming{false}; ///< True when playing a URL stream.
  class NetworkAudioStreamIO *fNetworkStream = nullptr;
  ///@}

  AudioRingBuffer fPrebufferRing;
  std::atomic<bool> fUsePrebuffer{false};
  std::atomic<bool> fPrebufferRunning{false};
  std::atomic<bool> fReaderEof{false};
  /// Seek waiting for the prebuffer thread, which owns fTrack; -1 if none.
  std::atomic<bigtime_t> fPendingSeek{-1};
  /// Set by the prebuffer thread after a seek: the callback drops the ring
  /// up to fFlushTo, i.e. the audio decoded before the seek, and takes
  /// fFlushPos as the position. fFlushSpin keeps the three consistent.
  std::atomic<bool> fFlushRequested{false};
  size_t fFlushTo = 0;
  bigtime_t fFlushPos = 0;
  std::atomic_flag fFlushSpin = ATOMIC_FLAG_INIT;
  thread_id fPrebufferThread = -1;
  int fPrebufferFrameSize = 4; ///< Native decoded-track frame size (bytes);
                                ///< set before the thread starts, read only
                                ///< by that thread while it runs.

  void _StartPrebufferThread(int frameSize);
  void _StopPrebufferThread();
  void _NotifyPlayFailed(const char *path);
  void _PrebufferThreadFunc();
  static int32 _PrebufferThreadEntry(void *cookie);

  /** @name Notification */
  ///@{
  BMessageRunner *fUpdateRunner = nullptr;
  BMessenger fTarget;
  ///@}

  BPrivate::Network::BUrlContext fUrlContext;
  BLocker fPlayLock;

  BUrl fCurrentUrl;
  BString fCurrentTitle;

#if ENABLE_LOCAL_OUTPUT
  AudioOutputManager fLocalOutputManager;
  OutputTarget fLocalOutputTarget = OutputTarget::SystemDefault;
  OutputBusInfo fLocalOutputBus;
  MixerConflictPolicy fLocalConflictPolicy = MixerConflictPolicy::Disconnect;
  BString fLocalFallbackDevice;
  // Resamples native decoded frames to a direct device's negotiated format.
  // Active only on the local-file direct path; passthrough/unset otherwise.
  AudioResampler fResampler;
  // Reusable source-frame scratch for _FillResampled, pre-sized in Play()
  // so the audio callback does not allocate on the hot path.
  std::vector<uint8> fResampleScratch;
  // Direct-output loudness match. In direct mode Beton applies fDirectGain in
  // software (in _ApplyFade). It replicates the Haiku mixer's TOTAL gain on
  // Beton's stream — the mixer's non-linear volume curve plus the master gain,
  // both of which a direct connection bypasses — so switching output modes
  // does not change loudness. Read on the audio thread; recomputed on the
  // control thread whenever fVolume or the captured master changes.
  std::atomic<float> fDirectGain{1.0f};
  std::atomic<float> fMixerMasterDb{0.0f};    // captured mixer master (displayed dB)
  std::atomic<bool> fDirectCompActive{false}; // true once a direct connect succeeds
  std::atomic<bool> fMixerAttenuate3dB{false}; // mixer's "-3 dB output" toggle state
  // Background device-warm thread (see _SpawnPrime). -1 when none outstanding.
  std::atomic<thread_id> fPrimeThread{-1};
#endif

#if ENABLE_DLNA_OUTPUT
  DLNAService *fDlnaManager = nullptr;
  LocalFileHttpServer *fLocalFileHttpServer = nullptr;
  std::atomic<bool> fIsRemotePlaying{false};
#endif
};

#endif // BETON_AUDIO_PLAYBACK_ENGINE_H
