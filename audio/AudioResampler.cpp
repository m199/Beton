/*
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "AudioResampler.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace {

/// Map a Haiku packed PCM format to the matching FFmpeg interleaved sample
/// format. BMediaTrack always hands back interleaved (packed) frames.
AVSampleFormat HaikuToAV(const media_raw_audio_format &f) {
  switch (f.format) {
  case media_raw_audio_format::B_AUDIO_FLOAT:
    return AV_SAMPLE_FMT_FLT;
  case media_raw_audio_format::B_AUDIO_INT:
    return AV_SAMPLE_FMT_S32;
  case media_raw_audio_format::B_AUDIO_SHORT:
    return AV_SAMPLE_FMT_S16;
  case media_raw_audio_format::B_AUDIO_UCHAR:
    return AV_SAMPLE_FMT_U8;
  default:
    return AV_SAMPLE_FMT_NONE;
  }
}

int BytesPerSample(const media_raw_audio_format &f) {
  return f.format & media_raw_audio_format::B_AUDIO_SIZE_MASK;
}

} // namespace

AudioResampler::AudioResampler()
    : fSwr(nullptr), fSrcRate(0), fDstRate(0), fSrcChannels(0), fDstChannels(0),
      fSrcFrameSize(0), fDstFrameSize(0) {}

AudioResampler::~AudioResampler() { Unset(); }

void AudioResampler::Unset() {
  if (fSwr) {
    swr_free(&fSwr);
    fSwr = nullptr;
  }
}

status_t AudioResampler::Init(const media_raw_audio_format &src,
                              const media_raw_audio_format &dst) {
  Unset();

  AVSampleFormat srcFmt = HaikuToAV(src);
  AVSampleFormat dstFmt = HaikuToAV(dst);
  if (srcFmt == AV_SAMPLE_FMT_NONE || dstFmt == AV_SAMPLE_FMT_NONE)
    return B_BAD_VALUE;
  if (src.frame_rate <= 0 || dst.frame_rate <= 0 || src.channel_count <= 0 ||
      dst.channel_count <= 0)
    return B_BAD_VALUE;

  fSrcRate = (int)src.frame_rate;
  fDstRate = (int)dst.frame_rate;
  fSrcChannels = src.channel_count;
  fDstChannels = dst.channel_count;
  fSrcFrameSize = BytesPerSample(src) * fSrcChannels;
  fDstFrameSize = BytesPerSample(dst) * fDstChannels;

  // Identical formats: nothing to convert. Leave fSwr null (passthrough).
  if (fSrcRate == fDstRate && fSrcChannels == fDstChannels && srcFmt == dstFmt)
    return B_OK;

  AVChannelLayout inLayout;
  AVChannelLayout outLayout;
  av_channel_layout_default(&inLayout, fSrcChannels);
  av_channel_layout_default(&outLayout, fDstChannels);

  int ret = swr_alloc_set_opts2(&fSwr, &outLayout, dstFmt, fDstRate, &inLayout,
                                srcFmt, fSrcRate, 0, nullptr);

  av_channel_layout_uninit(&inLayout);
  av_channel_layout_uninit(&outLayout);

  if (ret < 0 || fSwr == nullptr || swr_init(fSwr) < 0) {
    Unset();
    return B_ERROR;
  }
  return B_OK;
}

int64 AudioResampler::SourceFramesForOutput(int64 dstFrames) const {
  if (fSwr == nullptr || dstFrames <= 0)
    return 0;

  // Samples (in source-rate units) already buffered inside the converter.
  int64 delay = swr_get_delay(fSwr, fSrcRate);
  // ceil(dstFrames * srcRate / dstRate) - already-buffered.
  int64 srcNeeded =
      (dstFrames * (int64)fSrcRate + fDstRate - 1) / fDstRate - delay;
  return srcNeeded > 0 ? srcNeeded : 0;
}

int64 AudioResampler::Convert(const void *src, int64 srcFrames, void *dst,
                              int64 dstCapacityFrames) {
  if (fSwr == nullptr || dstCapacityFrames <= 0)
    return 0;

  const uint8_t *in = (const uint8_t *)src;
  uint8_t *out = (uint8_t *)dst;
  int outFrames = swr_convert(fSwr, &out, (int)dstCapacityFrames,
                              src != nullptr ? &in : nullptr, (int)srcFrames);
  return outFrames > 0 ? outFrames : 0;
}
