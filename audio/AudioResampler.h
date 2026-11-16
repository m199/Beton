/*
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#ifndef BETON_AUDIO_RESAMPLER_H
#define BETON_AUDIO_RESAMPLER_H

#include <MediaDefs.h>
#include <SupportDefs.h>

/**
 * @class AudioResampler
 * @brief Interleaved-PCM format converter built on libswresample.
 *
 * Converts a source media_raw_audio_format (sample rate, channel count and
 * sample format) to a destination format. Used on the local-output direct
 * path: BMediaTrack's decoder emits the file's native format and does NOT
 * resample, while a physical device node may run at a fixed, different rate
 * (e.g. 48/96 kHz). Feeding native-rate PCM straight to a faster device is
 * what produced the "chipmunk"/double-speed playback — see
 * LOCAL_AUDIO_OUTPUT_PLAN.md section 2.5.
 *
 * The system-mixer path does not need this (the mixer resamples), and the
 * network-stream path already resamples inside NetworkAudioStreamIO.
 */
class AudioResampler {
public:
  AudioResampler();
  ~AudioResampler();

  /**
   * @brief Configure conversion from @p src to @p dst.
   *
   * If the two formats are identical, no converter is created and IsActive()
   * stays false; callers should copy frames directly in that case.
   *
   * @return B_OK on success (including the identical/passthrough case),
   *         B_BAD_VALUE for an unsupported sample format, B_ERROR if
   *         libswresample failed to initialise.
   */
  status_t Init(const media_raw_audio_format &src,
                const media_raw_audio_format &dst);

  /** @brief Release the converter. IsActive() becomes false. */
  void Unset();

  /** @brief True when a real conversion is configured (not passthrough). */
  bool IsActive() const { return fSwr != nullptr; }

  /**
   * @brief Source frames to read to produce ~@p dstFrames output frames,
   *        accounting for samples already buffered inside the converter.
   */
  int64 SourceFramesForOutput(int64 dstFrames) const;

  /**
   * @brief Convert @p srcFrames interleaved source frames into @p dst.
   *
   * Writes at most @p dstCapacityFrames destination frames. Pass
   * @p src == NULL and @p srcFrames == 0 to flush the final tail at EOF.
   *
   * @return Destination frames written (>= 0).
   */
  int64 Convert(const void *src, int64 srcFrames, void *dst,
                int64 dstCapacityFrames);

  int SrcFrameSize() const { return fSrcFrameSize; } ///< Bytes per source frame.
  int DstFrameSize() const { return fDstFrameSize; } ///< Bytes per dst frame.

private:
  struct SwrContext *fSwr;
  int fSrcRate;
  int fDstRate;
  int fSrcChannels;
  int fDstChannels;
  int fSrcFrameSize;
  int fDstFrameSize;
};

#endif // BETON_AUDIO_RESAMPLER_H
