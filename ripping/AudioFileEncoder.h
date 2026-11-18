/*
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#ifndef BETON_AUDIO_FILE_ENCODER_H
#define BETON_AUDIO_FILE_ENCODER_H

#include "CDRipSettings.h"
#include "MediaItem.h"

#include <String.h>
#include <SupportDefs.h>

/**
 * @class AudioFileEncoder
 * @brief Writes 16-bit PCM into an audio file of one format, with tags.
 *
 * The ripper only talks to this interface. Today the one implementation uses
 * the FFmpeg libraries Beton already links (libavformat/libavcodec, LAME for
 * MP3); a lighter backend, such as a Media Kit or translator-based encoder,
 * can be dropped in behind CreateAudioFileEncoder() without touching the
 * ripper or its window.
 *
 * Call order: Open(), any number of Write(), then Finish() or Abort().
 * An encoder is used for one file only.
 */
class AudioFileEncoder {
public:
  virtual ~AudioFileEncoder() {}

  /**
   * @brief Creates @p path and prepares to encode.
   * @param path File to write; an existing file is replaced.
   * @param sampleRate Input sample rate in Hz.
   * @param channels Input channel count (1 or 2).
   * @param tags Metadata to embed (title, artist, album, track, ...).
   */
  virtual status_t Open(const char *path, int32 sampleRate, int32 channels,
                        const MediaItem &tags) = 0;

  /**
   * @brief Encodes @p frames frames of interleaved, native-endian 16-bit PCM.
   */
  virtual status_t Write(const int16 *samples, int32 frames) = 0;

  /** @brief Flushes the encoder and completes the file. */
  virtual status_t Finish() = 0;

  /** @brief Stops without completing the file; the caller deletes it. */
  virtual void Abort() = 0;

  /** @brief Human-readable reason for the last failure. */
  const BString &ErrorText() const { return fError; }

protected:
  BString fError;
};

/**
 * @brief Creates the encoder for @p format.
 * @return A new encoder owned by the caller; never null.
 */
AudioFileEncoder *CreateAudioFileEncoder(RipFormat format);

#endif // BETON_AUDIO_FILE_ENCODER_H
