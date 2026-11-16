/*
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#ifndef BETON_LOUDNESS_ANALYZER_H
#define BETON_LOUDNESS_ANALYZER_H

#include "LoudnessMeter.h"

#include <String.h>
#include <SupportDefs.h>

#include <atomic>
#include <memory>

/**
 * @namespace LoudnessAnalyzer
 * @brief Measures the loudness of an audio file by decoding it with FFmpeg.
 */
namespace LoudnessAnalyzer {

/**
 * @brief Decodes @p path and runs it through a LoudnessMeter.
 * @param cancel Checked between packets; may be null.
 * @param out Receives the meter (loudness, peak and blocks for album gain).
 * @param cancel2 A second cancel flag, also checked; may be null.
 * @return B_OK, B_CANCELED, or an error with @p error set.
 */
status_t AnalyzeFile(const char *path, const std::atomic<bool> *cancel,
                     std::unique_ptr<LoudnessMeter> &out, BString &error,
                     const std::atomic<bool> *cancel2 = nullptr);

} // namespace LoudnessAnalyzer

#endif // BETON_LOUDNESS_ANALYZER_H
