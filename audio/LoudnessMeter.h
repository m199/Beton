/*
 * Copyright 2026, David Beckman, indispensablelabs@gmail.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#ifndef BETON_LOUDNESS_METER_H
#define BETON_LOUDNESS_METER_H

#include <SupportDefs.h>

#include <vector>

/**
 * @class LoudnessMeter
 * @brief Integrated loudness (ITU-R BS.1770-4 / EBU R128) and sample peak of
 *        one track, fed samples as they are decoded or ripped.
 *
 * The signal is K-weighted, its mean square taken over 400 ms blocks that
 * overlap by 75 %, and the blocks gated: those below -70 LUFS are dropped,
 * then those more than 10 LU below the mean of the rest. The loudness of
 * what remains is the integrated loudness.
 *
 * ReplayGain 2.0 is derived from it: gain = -18 LUFS - loudness. Album
 * loudness gates the blocks of all the album's tracks together, so keep
 * each track's meter (or its Blocks()) until the album is done.
 */
class LoudnessMeter {
public:
  /** ReplayGain 2.0 reference level. */
  static constexpr double kReferenceLufs = -18.0;

  /** @param channels 1 or 2. Channels beyond two are ignored. */
  LoudnessMeter(int32 sampleRate, int32 channels);

  /** Interleaved 16-bit samples, native endian. */
  void AddFrames(const int16 *samples, size_t frames);
  /** Interleaved float samples, full scale = 1.0. */
  void AddFrames(const float *samples, size_t frames);

  /** Integrated loudness in LUFS; -HUGE_VAL for silence or < 400 ms. */
  double Loudness() const { return GatedLoudness(fBlocks); }
  /** Highest absolute sample value, full scale = 1.0. */
  double Peak() const { return fPeak; }
  /** ReplayGain 2.0 track gain in dB, or 0 when there is no loudness. */
  double Gain() const { return GainFor(Loudness()); }

  /** Mean-square energy of every 400 ms block, for album loudness. */
  const std::vector<double> &Blocks() const { return fBlocks; }

  /** Gated loudness of a set of blocks (one track, or a whole album). */
  static double GatedLoudness(const std::vector<double> &blocks);
  static double GainFor(double lufs);

private:
  struct Biquad {
    double b0, b1, b2, a1, a2;
    double z1 = 0, z2 = 0;
    double Process(double x) {
      // Transposed direct form II.
      double y = b0 * x + z1;
      z1 = b1 * x - a1 * y + z2;
      z2 = b2 * x - a2 * y;
      return y;
    }
  };

  void _AddFrame(double left, double right);

  int32 fChannels;
  int32 fStepFrames;  ///< 100 ms: blocks start every step.
  Biquad fShelf[2];   ///< K-weighting stage 1, per channel.
  Biquad fHighPass[2]; ///< K-weighting stage 2, per channel.

  double fStepSum = 0;  ///< Sum of weighted squares in the current step.
  int32 fStepCount = 0; ///< Frames in the current step.
  double fSteps[4] = {0, 0, 0, 0}; ///< The last four 100 ms sums.
  int32 fStepsSeen = 0;

  std::vector<double> fBlocks;
  double fPeak = 0;
};

#endif // BETON_LOUDNESS_METER_H
