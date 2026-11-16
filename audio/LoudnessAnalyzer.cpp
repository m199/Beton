/*
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "LoudnessAnalyzer.h"

#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/samplefmt.h>
}

namespace {

/** One sample as float, full scale = 1.0. */
float SampleAt(const AVFrame *f, int channel, int index, int channels) {
  const AVSampleFormat fmt = (AVSampleFormat)f->format;
  const bool planar = av_sample_fmt_is_planar(fmt) != 0;
  const int plane = planar ? channel : 0;
  const int i = planar ? index : index * channels + channel;
  const uint8 *data = f->extended_data[plane];
  switch (av_get_packed_sample_fmt(fmt)) {
  case AV_SAMPLE_FMT_U8:
    return (data[i] - 128) / 128.0f;
  case AV_SAMPLE_FMT_S16:
    return reinterpret_cast<const int16 *>(data)[i] / 32768.0f;
  case AV_SAMPLE_FMT_S32:
    return (float)(reinterpret_cast<const int32 *>(data)[i] / 2147483648.0);
  case AV_SAMPLE_FMT_FLT:
    return reinterpret_cast<const float *>(data)[i];
  case AV_SAMPLE_FMT_DBL:
    return (float)reinterpret_cast<const double *>(data)[i];
  default:
    return 0.0f;
  }
}

} // namespace

status_t LoudnessAnalyzer::AnalyzeFile(const char *path,
                                       const std::atomic<bool> *cancel,
                                       std::unique_ptr<LoudnessMeter> &out,
                                       BString &error,
                                       const std::atomic<bool> *cancel2) {
  out.reset();
  AVFormatContext *fmt = nullptr;
  int err = avformat_open_input(&fmt, path, nullptr, nullptr);
  if (err < 0) {
    error = "Could not open the file";
    return B_ERROR;
  }

  AVCodecContext *codec = nullptr;
  AVPacket *packet = nullptr;
  AVFrame *frame = nullptr;
  status_t status = B_OK;
  int stream = -1;
  int channels = 0;
  std::vector<float> interleaved;

  auto fail = [&](const char *what) {
    error = what;
    status = B_ERROR;
  };

  do {
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
      fail("Could not read the file's format");
      break;
    }
    stream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (stream < 0) {
      fail("The file has no audio");
      break;
    }
    const AVCodec *decoder =
        avcodec_find_decoder(fmt->streams[stream]->codecpar->codec_id);
    codec = decoder ? avcodec_alloc_context3(decoder) : nullptr;
    if (codec == nullptr ||
        avcodec_parameters_to_context(codec, fmt->streams[stream]->codecpar) < 0 ||
        avcodec_open2(codec, decoder, nullptr) < 0) {
      fail("No decoder for this file");
      break;
    }

    // BS.1770 weighs left and right equally; further channels (surround)
    // are left out, which keeps stereo music exact.
    channels = codec->ch_layout.nb_channels >= 2 ? 2 : 1;
    out.reset(new LoudnessMeter(codec->sample_rate, channels));
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (packet == nullptr || frame == nullptr) {
      fail("Out of memory");
      break;
    }

    const int sourceChannels = codec->ch_layout.nb_channels;
    auto drain = [&]() {
      while (avcodec_receive_frame(codec, frame) == 0) {
        const int n = frame->nb_samples;
        interleaved.resize((size_t)n * channels);
        for (int i = 0; i < n; i++)
          for (int c = 0; c < channels; c++)
            interleaved[(size_t)i * channels + c] =
                SampleAt(frame, c, i, sourceChannels);
        out->AddFrames(interleaved.data(), n);
        av_frame_unref(frame);
      }
    };

    while (av_read_frame(fmt, packet) >= 0) {
      if (packet->stream_index == stream) {
        avcodec_send_packet(codec, packet);
        drain();
      }
      av_packet_unref(packet);
      if ((cancel != nullptr && cancel->load()) ||
          (cancel2 != nullptr && cancel2->load())) {
        status = B_CANCELED;
        break;
      }
    }
    if (status == B_OK) {
      avcodec_send_packet(codec, nullptr);
      drain();
    }
  } while (false);

  av_frame_free(&frame);
  av_packet_free(&packet);
  avcodec_free_context(&codec);
  avformat_close_input(&fmt);
  if (status != B_OK)
    out.reset();
  return status;
}
