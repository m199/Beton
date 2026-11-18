/*
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

/**
 * @file FFmpegAudioEncoder.cpp
 * @brief AudioFileEncoder on top of libavformat/libavcodec.
 *
 * Uses the FFmpeg libraries in-process rather than running the ffmpeg tool,
 * so ripping needs nothing beyond the libraries Beton already depends on.
 * Haiku's ffmpeg build carries the LAME encoder (libmp3lame) for MP3; FLAC and
 * PCM are FFmpeg's own encoders.
 */

#include "AudioFileEncoder.h"

#include <string.h>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}

namespace {

BString AvError(int err) {
  char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
  av_strerror(err, buf, sizeof(buf));
  return BString(buf);
}

class FFmpegAudioEncoder : public AudioFileEncoder {
public:
  explicit FFmpegAudioEncoder(RipFormat format) : fFormat(format) {}
  ~FFmpegAudioEncoder() override { _Close(); }

  status_t Open(const char *path, int32 sampleRate, int32 channels,
                const MediaItem &tags) override;
  status_t Write(const int16 *samples, int32 frames) override;
  status_t Finish() override;
  void Abort() override { _Close(); }

private:
  status_t _Fail(const char *what, int err);
  void _SetTags(const MediaItem &tags);
  status_t _EncodeFrame(const int16 *samples, int32 frames);
  status_t _WritePackets();
  void _Close();

  RipFormat fFormat;
  AVFormatContext *fMux = nullptr;
  AVCodecContext *fCodec = nullptr;
  AVStream *fStream = nullptr;
  AVFrame *fFrame = nullptr;
  AVPacket *fPacket = nullptr;

  int32 fChannels = 0;
  int32 fFrameSize = 0;        ///< Samples per channel the encoder takes per frame.
  bool fPlanar = false;        ///< Encoder wants one plane per channel (LAME).
  bool fShortLastFrame = true; ///< Encoder accepts a final frame below fFrameSize.
  int64 fNextPts = 0;
  std::vector<int16> fPending; ///< Interleaved samples not yet a whole frame.
};

status_t FFmpegAudioEncoder::_Fail(const char *what, int err) {
  fError = what;
  if (err < 0)
    fError << ": " << AvError(err);
  _Close();
  return B_ERROR;
}

void FFmpegAudioEncoder::_SetTags(const MediaItem &t) {
  AVDictionary **md = &fMux->metadata;
  auto set = [md](const char *key, const BString &value) {
    if (!value.IsEmpty())
      av_dict_set(md, key, value.String(), 0);
  };

  set("title", t.title);
  set("artist", t.artist);
  set("album", t.album);
  set("album_artist", t.albumArtist);
  set("genre", t.genre);
  set("composer", t.composer);
  set("comment", t.comment);

  BString number;
  if (t.track > 0) {
    number << t.track;
    if (t.trackTotal > 0)
      number << "/" << t.trackTotal;
    set("track", number);
  }
  if (t.disc > 0) {
    number.Truncate(0);
    number << t.disc;
    if (t.discTotal > 0)
      number << "/" << t.discTotal;
    set("disc", number);
  }
  if (t.year > 0) {
    number.Truncate(0);
    number << t.year;
    set("date", number);
  }
}

status_t FFmpegAudioEncoder::Open(const char *path, int32 sampleRate,
                                  int32 channels, const MediaItem &tags) {
  _Close();
  fError.Truncate(0);

  const char *muxer = nullptr;
  const AVCodec *codec = nullptr;
  AVSampleFormat sampleFormat = AV_SAMPLE_FMT_S16;

  switch (fFormat) {
  case RipFormat::MP3_320:
    muxer = "mp3";
    codec = avcodec_find_encoder_by_name("libmp3lame");
    sampleFormat = AV_SAMPLE_FMT_S16P;
    break;
  case RipFormat::FLAC:
    muxer = "flac";
    codec = avcodec_find_encoder(AV_CODEC_ID_FLAC);
    sampleFormat = AV_SAMPLE_FMT_S16;
    break;
  case RipFormat::WAV:
    muxer = "wav";
    codec = avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE);
    sampleFormat = AV_SAMPLE_FMT_S16;
    break;
  }

  if (codec == nullptr) {
    fError.SetToFormat("FFmpeg has no %s encoder installed",
                       RipFormatExtension(fFormat));
    return B_NOT_SUPPORTED;
  }

  int err = avformat_alloc_output_context2(&fMux, nullptr, muxer, path);
  if (err < 0 || fMux == nullptr)
    return _Fail("Could not set up the output file", err);

  fStream = avformat_new_stream(fMux, nullptr);
  fCodec = avcodec_alloc_context3(codec);
  if (fStream == nullptr || fCodec == nullptr)
    return _Fail("Out of memory", 0);

  fChannels = channels;
  fCodec->sample_fmt = sampleFormat;
  fCodec->sample_rate = sampleRate;
  fCodec->time_base = AVRational{1, (int)sampleRate};
  av_channel_layout_default(&fCodec->ch_layout, channels);

  if (fFormat == RipFormat::MP3_320) {
    fCodec->bit_rate = 320000;
    fCodec->compression_level = 2; // LAME -q 2: the recommended high quality.
  } else if (fFormat == RipFormat::FLAC) {
    // Lossless at any level; 8 is the smallest file and still far faster
    // than the drive can read.
    fCodec->compression_level = 8;
  }

  if (fMux->oformat->flags & AVFMT_GLOBALHEADER)
    fCodec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  err = avcodec_open2(fCodec, codec, nullptr);
  if (err < 0)
    return _Fail("Could not start the encoder", err);

  err = avcodec_parameters_from_context(fStream->codecpar, fCodec);
  if (err < 0)
    return _Fail("Could not set up the output stream", err);
  fStream->time_base = fCodec->time_base;

  _SetTags(tags);

  if (!(fMux->oformat->flags & AVFMT_NOFILE)) {
    err = avio_open(&fMux->pb, path, AVIO_FLAG_WRITE);
    if (err < 0)
      return _Fail("Could not create the file", err);
  }

  err = avformat_write_header(fMux, nullptr);
  if (err < 0)
    return _Fail("Could not write the file header", err);

  const bool variable =
      (codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE) != 0;
  fFrameSize = (fCodec->frame_size > 0 && !variable) ? fCodec->frame_size : 4096;
  fShortLastFrame =
      variable || (codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME) != 0;
  fPlanar = av_sample_fmt_is_planar(sampleFormat) != 0;

  fFrame = av_frame_alloc();
  fPacket = av_packet_alloc();
  if (fFrame == nullptr || fPacket == nullptr)
    return _Fail("Out of memory", 0);

  fFrame->format = sampleFormat;
  fFrame->sample_rate = sampleRate;
  fFrame->nb_samples = fFrameSize;
  err = av_channel_layout_copy(&fFrame->ch_layout, &fCodec->ch_layout);
  if (err >= 0)
    err = av_frame_get_buffer(fFrame, 0);
  if (err < 0)
    return _Fail("Out of memory", err);

  fNextPts = 0;
  fPending.clear();
  fPending.reserve((size_t)fFrameSize * channels * 2);
  return B_OK;
}

status_t FFmpegAudioEncoder::_WritePackets() {
  for (;;) {
    int err = avcodec_receive_packet(fCodec, fPacket);
    if (err == AVERROR(EAGAIN) || err == AVERROR_EOF)
      return B_OK;
    if (err < 0)
      return _Fail("Encoding failed", err);

    av_packet_rescale_ts(fPacket, fCodec->time_base, fStream->time_base);
    fPacket->stream_index = fStream->index;
    err = av_interleaved_write_frame(fMux, fPacket);
    if (err < 0)
      return _Fail("Could not write to the file", err);
  }
}

status_t FFmpegAudioEncoder::_EncodeFrame(const int16 *samples, int32 frames) {
  int err = av_frame_make_writable(fFrame);
  if (err < 0)
    return _Fail("Out of memory", err);

  fFrame->nb_samples = frames;
  if (fPlanar) {
    for (int32 ch = 0; ch < fChannels; ch++) {
      int16 *dst = reinterpret_cast<int16 *>(fFrame->data[ch]);
      for (int32 i = 0; i < frames; i++)
        dst[i] = samples[i * fChannels + ch];
    }
  } else {
    memcpy(fFrame->data[0], samples, (size_t)frames * fChannels * sizeof(int16));
  }

  fFrame->pts = fNextPts;
  fNextPts += frames;

  err = avcodec_send_frame(fCodec, fFrame);
  if (err < 0)
    return _Fail("Encoding failed", err);
  return _WritePackets();
}

status_t FFmpegAudioEncoder::Write(const int16 *samples, int32 frames) {
  if (fCodec == nullptr)
    return B_NO_INIT;

  fPending.insert(fPending.end(), samples, samples + (size_t)frames * fChannels);

  const size_t perFrame = (size_t)fFrameSize * fChannels;
  size_t used = 0;
  while (fPending.size() - used >= perFrame) {
    status_t status = _EncodeFrame(fPending.data() + used, fFrameSize);
    if (status != B_OK)
      return status;
    used += perFrame;
  }
  fPending.erase(fPending.begin(), fPending.begin() + used);
  return B_OK;
}

status_t FFmpegAudioEncoder::Finish() {
  if (fCodec == nullptr)
    return B_NO_INIT;

  if (!fPending.empty()) {
    int32 frames = (int32)(fPending.size() / fChannels);
    if (!fShortLastFrame) {
      // Pad with silence rather than drop the last few milliseconds.
      fPending.resize((size_t)fFrameSize * fChannels, 0);
      frames = fFrameSize;
    }
    status_t status = _EncodeFrame(fPending.data(), frames);
    if (status != B_OK)
      return status;
    fPending.clear();
  }

  int err = avcodec_send_frame(fCodec, nullptr);
  if (err < 0)
    return _Fail("Encoding failed", err);
  status_t status = _WritePackets();
  if (status != B_OK)
    return status;

  err = av_write_trailer(fMux);
  if (err < 0)
    return _Fail("Could not finish the file", err);

  _Close();
  return B_OK;
}

void FFmpegAudioEncoder::_Close() {
  if (fMux != nullptr) {
    if (!(fMux->oformat->flags & AVFMT_NOFILE) && fMux->pb != nullptr)
      avio_closep(&fMux->pb);
    avformat_free_context(fMux);
    fMux = nullptr;
  }
  fStream = nullptr;
  avcodec_free_context(&fCodec);
  av_frame_free(&fFrame);
  av_packet_free(&fPacket);
  fPending.clear();
}

} // namespace

AudioFileEncoder *CreateAudioFileEncoder(RipFormat format) {
  return new FFmpegAudioEncoder(format);
}
