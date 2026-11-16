/*
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "LoudnessMeter.h"

#include <math.h>

namespace {

// BS.1770 K-weighting as analogue prototypes, so the filters fit any sample
// rate. These are the parameters libebur128 uses; at 48 kHz they give the
// coefficients tabled in BS.1770.
const double kShelfFreq = 1681.974450955533;
const double kShelfGainDb = 3.999843853973347;
const double kShelfQ = 0.7071752369554196;
const double kHighPassFreq = 38.13547087602444;
const double kHighPassQ = 0.5003270373238773;

const double kAbsoluteGate = -70.0; // LUFS
const double kRelativeGate = -10.0; // LU below the ungated mean

double EnergyToLufs(double energy) { return -0.691 + 10.0 * log10(energy); }
double LufsToEnergy(double lufs) { return pow(10.0, (lufs + 0.691) / 10.0); }

} // namespace

LoudnessMeter::LoudnessMeter(int32 sampleRate, int32 channels)
    : fChannels(channels < 1 ? 1 : channels),
      fStepFrames(sampleRate / 10 > 0 ? sampleRate / 10 : 1) {
  const double fs = sampleRate > 0 ? sampleRate : 44100;

  {
    const double k = tan(M_PI * kShelfFreq / fs);
    const double vh = pow(10.0, kShelfGainDb / 20.0);
    const double vb = pow(vh, 0.4996667741545416);
    const double a0 = 1.0 + k / kShelfQ + k * k;
    Biquad shelf;
    shelf.b0 = (vh + vb * k / kShelfQ + k * k) / a0;
    shelf.b1 = 2.0 * (k * k - vh) / a0;
    shelf.b2 = (vh - vb * k / kShelfQ + k * k) / a0;
    shelf.a1 = 2.0 * (k * k - 1.0) / a0;
    shelf.a2 = (1.0 - k / kShelfQ + k * k) / a0;
    fShelf[0] = fShelf[1] = shelf;
  }
  {
    const double k = tan(M_PI * kHighPassFreq / fs);
    const double a0 = 1.0 + k / kHighPassQ + k * k;
    Biquad hp;
    hp.b0 = 1.0;
    hp.b1 = -2.0;
    hp.b2 = 1.0;
    hp.a1 = 2.0 * (k * k - 1.0) / a0;
    hp.a2 = (1.0 - k / kHighPassQ + k * k) / a0;
    fHighPass[0] = fHighPass[1] = hp;
  }
}

void LoudnessMeter::_AddFrame(double left, double right) {
  const double al = fabs(left);
  if (al > fPeak)
    fPeak = al;
  double sum;
  double l = fHighPass[0].Process(fShelf[0].Process(left));
  if (fChannels >= 2) {
    const double ar = fabs(right);
    if (ar > fPeak)
      fPeak = ar;
    double r = fHighPass[1].Process(fShelf[1].Process(right));
    // Left and right both weigh 1.0 in BS.1770.
    sum = l * l + r * r;
  } else {
    sum = l * l;
  }

  fStepSum += sum;
  if (++fStepCount < fStepFrames)
    return;

  // A 100 ms step is complete: the block is the last four steps.
  fSteps[0] = fSteps[1];
  fSteps[1] = fSteps[2];
  fSteps[2] = fSteps[3];
  fSteps[3] = fStepSum;
  fStepSum = 0;
  fStepCount = 0;
  if (++fStepsSeen >= 4) {
    const double energy =
        (fSteps[0] + fSteps[1] + fSteps[2] + fSteps[3]) / (4.0 * fStepFrames);
    fBlocks.push_back(energy);
  }
}

void LoudnessMeter::AddFrames(const int16 *samples, size_t frames) {
  const double scale = 1.0 / 32768.0;
  for (size_t i = 0; i < frames; i++) {
    const int16 *f = samples + i * fChannels;
    _AddFrame(f[0] * scale, fChannels >= 2 ? f[1] * scale : 0.0);
  }
}

void LoudnessMeter::AddFrames(const float *samples, size_t frames) {
  for (size_t i = 0; i < frames; i++) {
    const float *f = samples + i * fChannels;
    _AddFrame(f[0], fChannels >= 2 ? f[1] : 0.0);
  }
}

double LoudnessMeter::GatedLoudness(const std::vector<double> &blocks) {
  const double absolute = LufsToEnergy(kAbsoluteGate);
  double sum = 0;
  size_t count = 0;
  for (double e : blocks) {
    if (e > absolute) {
      sum += e;
      count++;
    }
  }
  if (count == 0)
    return -HUGE_VAL;

  const double relative =
      LufsToEnergy(EnergyToLufs(sum / count) + kRelativeGate);
  const double gate = relative > absolute ? relative : absolute;
  sum = 0;
  count = 0;
  for (double e : blocks) {
    if (e > gate) {
      sum += e;
      count++;
    }
  }
  return count > 0 ? EnergyToLufs(sum / count) : -HUGE_VAL;
}

double LoudnessMeter::GainFor(double lufs) {
  if (!isfinite(lufs))
    return 0.0;
  return kReferenceLufs - lufs;
}
