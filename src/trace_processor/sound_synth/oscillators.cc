/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_processor/sound_synth/oscillators.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace perfetto::trace_processor::sound_synth {

namespace {

constexpr double kTwoPi = 2.0 * 3.14159265358979323846;

// PolyBLEP: 2-sample polynomial residual for a unit-amplitude step
// discontinuity located inside the current sample. |t| is the current
// phase (in [0,1)) and |dt| is the phase increment per sample (freq/sr).
//
// For a falling edge, the caller *subtracts* this residual from the naive
// waveform. For a rising edge, the caller *adds* it.
//
// Derivation: see Välimäki & Huovilainen "Antialiasing Oscillators in
// Subtractive Synthesis" (2007). The residual is the difference between
// the band-limited step (integral of a bandlimited impulse) and the naive
// step, approximated by a quadratic over ±1 sample.
inline double PolyBlep(double t, double dt) {
  if (t < dt) {
    t /= dt;
    return t + t - t * t - 1.0;
  }
  if (t > 1.0 - dt) {
    t = (t - 1.0) / dt;
    return t * t + t + t + 1.0;
  }
  return 0.0;
}

}  // namespace

// --- ClassicOsc ---

ClassicOsc::ClassicOsc(Waveform waveform,
                       double base_freq_hz,
                       double pulse_width)
    : waveform_(waveform),
      base_freq_hz_(base_freq_hz),
      pulse_width_(std::clamp(pulse_width, 0.05, 0.95)) {
  out_ = AddOutput("out");
}

void ClassicOsc::Process(uint32_t num_samples) {
  out_->resize(num_samples);
  const SignalBuffer* freq_in = GetInput("freq");
  const SignalBuffer* freq_mod = GetInput("freq_mod");
  const SignalBuffer* reset = GetInput("reset");
  const double inv_sr = 1.0 / kSampleRate;

  for (uint32_t i = 0; i < num_samples; ++i) {
    // Resolve frequency for this sample.
    double freq = freq_in ? static_cast<double>((*freq_in)[i]) : base_freq_hz_;
    if (freq_mod)
      freq += static_cast<double>((*freq_mod)[i]);
    // Clamp to sane range. Negative or zero frequencies would break polyBLEP.
    if (freq < 0.1)
      freq = 0.1;
    if (freq > kSampleRate * 0.45)
      freq = kSampleRate * 0.45;
    const double dt = freq * inv_sr;

    // Rising-edge reset (for kick drums).
    if (reset) {
      bool high = (*reset)[i] > 0.5f;
      if (high && !reset_prev_high_) {
        phase_ = 0.0;
      }
      reset_prev_high_ = high;
    }

    // Advance phase.
    phase_ += dt;
    if (phase_ >= 1.0)
      phase_ -= 1.0;

    double sample = 0.0;
    switch (waveform_) {
      case Waveform::kSine:
        sample = std::sin(kTwoPi * phase_);
        break;
      case Waveform::kSaw: {
        // Naive downward-stepping sawtooth, ramps -1 → +1 over a cycle.
        // Discontinuity at phase wrap is a -2 step → subtract polyBLEP.
        sample = 2.0 * phase_ - 1.0 - PolyBlep(phase_, dt);
        break;
      }
      case Waveform::kSquare: {
        // Square with pulse-width. Rising edge at phase=0, falling edge at
        // phase=pw. Both corrections applied via polyBLEP.
        double naive = phase_ < pulse_width_ ? 1.0 : -1.0;
        // Rising edge at 0 (signed +2, so ADD polyBLEP).
        double blep_rise = PolyBlep(phase_, dt);
        // Falling edge at pw (signed -2, so SUBTRACT polyBLEP around pw).
        double t_fall = phase_ - pulse_width_;
        if (t_fall < 0.0)
          t_fall += 1.0;
        double blep_fall = PolyBlep(t_fall, dt);
        sample = naive + blep_rise - blep_fall;
        break;
      }
      case Waveform::kTriangle: {
        // Direct piecewise-linear triangle in [-1, 1]. Peak at phase=0,
        // valley at phase=0.5.
        //   phase=0.00 → +1     phase=0.25 →  0
        //   phase=0.50 → -1     phase=0.75 →  0
        // Naive (no anti-aliasing); at typical bass/mid techno pitches the
        // harmonic series is compact enough that aliasing is inaudible.
        // TODO: add polyBLAMP correction at peak/valley if needed at high
        // pitches.
        sample = 2.0 * std::abs(2.0 * phase_ - 1.0) - 1.0;
        break;
      }
    }
    (*out_)[i] = static_cast<float>(sample);
  }
}

// --- FmOsc ---

FmOsc::FmOsc(double base_freq_hz,
             double mod_ratio,
             double mod_index,
             double feedback)
    : base_freq_hz_(base_freq_hz),
      mod_ratio_(std::clamp(mod_ratio, 0.0, 16.0)),
      mod_index_(std::clamp(mod_index, 0.0, 32.0)),
      feedback_(std::clamp(feedback, 0.0, 1.0)) {
  out_ = AddOutput("out");
}

void FmOsc::Process(uint32_t num_samples) {
  out_->resize(num_samples);
  const SignalBuffer* freq_in = GetInput("freq");
  const SignalBuffer* freq_mod = GetInput("freq_mod");
  const SignalBuffer* index_mod = GetInput("index_mod");
  const double inv_sr = 1.0 / kSampleRate;
  for (uint32_t i = 0; i < num_samples; ++i) {
    double freq = freq_in ? static_cast<double>((*freq_in)[i]) : base_freq_hz_;
    if (freq_mod)
      freq += static_cast<double>((*freq_mod)[i]);
    freq = std::clamp(freq, 0.1, kSampleRate * 0.45);
    double index = mod_index_;
    if (index_mod)
      index += static_cast<double>((*index_mod)[i]);

    // Modulator: sin(2π·mod_phase + feedback*last_mod).
    mod_phase_ += freq * mod_ratio_ * inv_sr;
    if (mod_phase_ >= 1.0)
      mod_phase_ -= std::floor(mod_phase_);
    double mod_out = std::sin(kTwoPi * mod_phase_ + feedback_ * last_mod_);
    last_mod_ = mod_out;

    // Carrier: phase-modulated by the modulator.
    car_phase_ += freq * inv_sr;
    if (car_phase_ >= 1.0)
      car_phase_ -= std::floor(car_phase_);
    double sample = std::sin(kTwoPi * car_phase_ + index * mod_out);

    (*out_)[i] = static_cast<float>(sample);
  }
}

// --- PhaseDistortionOsc ---

PhaseDistortionOsc::PhaseDistortionOsc(Mode mode,
                                       double base_freq_hz,
                                       double amount)
    : mode_(mode),
      base_freq_hz_(base_freq_hz),
      amount_(std::clamp(amount, 0.0, 1.0)) {
  out_ = AddOutput("out");
}

void PhaseDistortionOsc::Process(uint32_t num_samples) {
  out_->resize(num_samples);
  const SignalBuffer* freq_in = GetInput("freq");
  const SignalBuffer* freq_mod = GetInput("freq_mod");
  const SignalBuffer* amount_mod = GetInput("amount_mod");
  const double inv_sr = 1.0 / kSampleRate;
  for (uint32_t i = 0; i < num_samples; ++i) {
    double freq = freq_in ? static_cast<double>((*freq_in)[i]) : base_freq_hz_;
    if (freq_mod)
      freq += static_cast<double>((*freq_mod)[i]);
    freq = std::clamp(freq, 0.1, kSampleRate * 0.45);
    phase_ += freq * inv_sr;
    if (phase_ >= 1.0)
      phase_ -= 1.0;

    double amt = amount_;
    if (amount_mod)
      amt += static_cast<double>((*amount_mod)[i]);
    amt = std::clamp(amt, 0.0, 0.999);

    double warped;
    switch (mode_) {
      case Mode::kSawWarp: {
        // Accelerate phase through the first portion, then slow down.
        // threshold shrinks as amount → 1.
        double threshold = 0.5 - 0.5 * amt;
        if (threshold < 1e-6)
          threshold = 1e-6;
        if (phase_ < threshold) {
          warped = phase_ * 0.5 / threshold;
        } else {
          warped = 0.5 + (phase_ - threshold) * 0.5 / (1.0 - threshold);
        }
        break;
      }
      case Mode::kPulseWarp: {
        // Edge region gets compressed, central region holds flat.
        double edge = 0.5 - 0.5 * amt;
        if (edge < 1e-6)
          edge = 1e-6;
        if (phase_ < edge) {
          warped = phase_ * 0.25 / edge;
        } else if (phase_ < 0.5) {
          warped = 0.25;
        } else if (phase_ < 0.5 + edge) {
          warped = 0.25 + (phase_ - 0.5) * 0.25 / edge;
        } else {
          warped = 0.5;
        }
        break;
      }
    }
    (*out_)[i] = static_cast<float>(std::sin(kTwoPi * warped));
  }
}

// --- FoldOsc ---

FoldOsc::FoldOsc(double base_freq_hz, double drive, double bias)
    : base_freq_hz_(base_freq_hz),
      drive_(std::clamp(drive, 1.0, 20.0)),
      bias_(std::clamp(bias, -1.0, 1.0)) {
  out_ = AddOutput("out");
}

void FoldOsc::Process(uint32_t num_samples) {
  out_->resize(num_samples);
  const SignalBuffer* freq_in = GetInput("freq");
  const SignalBuffer* freq_mod = GetInput("freq_mod");
  const SignalBuffer* drive_mod = GetInput("drive_mod");
  const double inv_sr = 1.0 / kSampleRate;
  for (uint32_t i = 0; i < num_samples; ++i) {
    double freq = freq_in ? static_cast<double>((*freq_in)[i]) : base_freq_hz_;
    if (freq_mod)
      freq += static_cast<double>((*freq_mod)[i]);
    freq = std::clamp(freq, 0.1, kSampleRate * 0.45);
    double drive = drive_;
    if (drive_mod)
      drive += static_cast<double>((*drive_mod)[i]);
    drive = std::clamp(drive, 1.0, 20.0);

    phase_ += freq * inv_sr;
    if (phase_ >= 1.0)
      phase_ -= 1.0;

    // Smooth fold: sin(drive * (sin(2π·phase) + bias)).
    double inner = std::sin(kTwoPi * phase_) + bias_;
    double sample = std::sin(drive * inner);
    (*out_)[i] = static_cast<float>(sample);
  }
}

// --- SyncOsc ---

SyncOsc::SyncOsc(double base_freq_hz, double sync_ratio)
    : base_freq_hz_(base_freq_hz),
      sync_ratio_(std::clamp(sync_ratio, 1.0, 16.0)) {
  out_ = AddOutput("out");
}

void SyncOsc::Process(uint32_t num_samples) {
  out_->resize(num_samples);
  const SignalBuffer* freq_in = GetInput("freq");
  const SignalBuffer* freq_mod = GetInput("freq_mod");
  const SignalBuffer* ratio_mod = GetInput("ratio_mod");
  const double inv_sr = 1.0 / kSampleRate;
  for (uint32_t i = 0; i < num_samples; ++i) {
    double freq = freq_in ? static_cast<double>((*freq_in)[i]) : base_freq_hz_;
    if (freq_mod)
      freq += static_cast<double>((*freq_mod)[i]);
    freq = std::clamp(freq, 0.1, kSampleRate * 0.45);

    double ratio = sync_ratio_;
    if (ratio_mod)
      ratio += static_cast<double>((*ratio_mod)[i]);
    ratio = std::clamp(ratio, 1.0, 16.0);

    // Advance master.
    master_phase_ += freq * inv_sr;
    if (master_phase_ >= 1.0) {
      master_phase_ -= 1.0;
      slave_phase_ = 0.0;  // Hard sync reset.
    }
    // Advance slave.
    double slave_dt = freq * ratio * inv_sr;
    slave_phase_ += slave_dt;
    if (slave_phase_ >= 1.0)
      slave_phase_ -= 1.0;

    // polyBLEP saw on slave (ignoring the sync-reset discontinuity).
    double sample = 2.0 * slave_phase_ - 1.0 - PolyBlep(slave_phase_, slave_dt);
    (*out_)[i] = static_cast<float>(sample);
  }
}

// --- SuperOsc ---

SuperOsc::SuperOsc(double base_freq_hz, double detune, double mix)
    : base_freq_hz_(base_freq_hz),
      detune_(std::clamp(detune, 0.0, 1.0)),
      mix_(std::clamp(mix, 0.0, 1.0)) {
  out_ = AddOutput("out");
}

void SuperOsc::Process(uint32_t num_samples) {
  out_->resize(num_samples);
  const SignalBuffer* freq_in = GetInput("freq");
  const SignalBuffer* freq_mod = GetInput("freq_mod");
  const double inv_sr = 1.0 / kSampleRate;
  // Semitone offsets for the 7 oscillators: -3, -2, -1, 0, +1, +2, +3.
  static const int kOffsets[7] = {-3, -2, -1, 0, 1, 2, 3};
  // Detune in semitones (simple linear mapping for v1).
  const double detune_semi = detune_ * 0.6;

  for (uint32_t i = 0; i < num_samples; ++i) {
    double freq = freq_in ? static_cast<double>((*freq_in)[i]) : base_freq_hz_;
    if (freq_mod)
      freq += static_cast<double>((*freq_mod)[i]);
    freq = std::clamp(freq, 0.1, kSampleRate * 0.45);

    double center_sample = 0.0;
    double side_sum = 0.0;
    for (int k = 0; k < 7; ++k) {
      double f = freq * std::pow(2.0, (kOffsets[k] * detune_semi) / 12.0);
      double dt = f * inv_sr;
      phases_[k] += dt;
      if (phases_[k] >= 1.0)
        phases_[k] -= 1.0;
      double s = 2.0 * phases_[k] - 1.0 - PolyBlep(phases_[k], dt);
      if (k == 3)
        center_sample = s;
      else
        side_sum += s;
    }
    // Equal-energy style mix: sides are normalized by sqrt(6).
    double sample =
        (1.0 - mix_) * center_sample + mix_ * side_sum * (1.0 / std::sqrt(6.0));
    (*out_)[i] = static_cast<float>(sample);
  }
}

// --- DrawbarOrgan ---

DrawbarOrgan::DrawbarOrgan(double base_freq_hz, const double levels[9])
    : base_freq_hz_(base_freq_hz), level_sum_(0.0) {
  for (uint32_t i = 0; i < kNumDrawbars; ++i) {
    levels_[i] = std::clamp(levels[i], 0.0, 1.0);
    level_sum_ += levels_[i];
  }
  // Normalize so all drawbars fully out still produces unit amplitude.
  if (level_sum_ < 1.0)
    level_sum_ = 1.0;
  out_ = AddOutput("out");
}

void DrawbarOrgan::Process(uint32_t num_samples) {
  out_->resize(num_samples);
  const SignalBuffer* freq_in = GetInput("freq");
  const SignalBuffer* freq_mod = GetInput("freq_mod");
  const double inv_sr = 1.0 / kSampleRate;
  const double norm = 1.0 / level_sum_;
  for (uint32_t i = 0; i < num_samples; ++i) {
    double freq = freq_in ? static_cast<double>((*freq_in)[i]) : base_freq_hz_;
    if (freq_mod)
      freq += static_cast<double>((*freq_mod)[i]);
    freq = std::clamp(freq, 0.1, kSampleRate * 0.45);
    // Advance each drawbar's phase and sum the contributions.
    double sample = 0.0;
    for (uint32_t d = 0; d < kNumDrawbars; ++d) {
      double partial_f = freq * kRatios[d];
      // Anti-aliasing: silently drop partials above Nyquist.
      if (partial_f >= kSampleRate * 0.45) {
        continue;
      }
      phases_[d] += partial_f * inv_sr;
      if (phases_[d] >= 1.0)
        phases_[d] -= std::floor(phases_[d]);
      sample += levels_[d] * std::sin(kTwoPi * phases_[d]);
    }
    (*out_)[i] = static_cast<float>(sample * norm);
  }
}

// --- WavetableOsc ---

WavetableOsc::WavetableOsc(TableType table_type,
                           double base_freq_hz,
                           double base_position)
    : table_type_(table_type),
      base_freq_hz_(base_freq_hz),
      base_position_(std::clamp(base_position, 0.0, 1.0)) {
  out_ = AddOutput("out");
  BuildTable();
}

void WavetableOsc::BuildTable() {
  table_.assign(kNumFrames * kFrameSize, 0.0f);
  // Per-frame generation. Formulas chosen so adjacent frames morph
  // smoothly — this is what makes the position knob musical.
  for (uint32_t f = 0; f < kNumFrames; ++f) {
    const double pos = static_cast<double>(f) / (kNumFrames - 1);  // 0..1
    double* frame = nullptr;
    (void)frame;  // suppress unused warning
    float* frame_p = &table_[f * kFrameSize];

    switch (table_type_) {
      case TableType::kSineToSaw: {
        // Sum sin(2π·h·x)/h^α, with α interpolating from 4 (very "sine-
        // like", only the fundamental audible) down to 1 (pure saw).
        const double alpha = 4.0 - 3.0 * pos;
        constexpr int kH = 48;
        double peak = 0.0;
        for (uint32_t n = 0; n < kFrameSize; ++n) {
          double x = static_cast<double>(n) / kFrameSize;
          double y = 0.0;
          for (int h = 1; h <= kH; ++h) {
            y += std::sin(kTwoPi * h * x) /
                 std::pow(static_cast<double>(h), alpha);
          }
          frame_p[n] = static_cast<float>(y);
          if (std::abs(y) > peak)
            peak = std::abs(y);
        }
        // Normalize to unit amplitude.
        if (peak > 1e-9) {
          float scale = static_cast<float>(1.0 / peak);
          for (uint32_t n = 0; n < kFrameSize; ++n)
            frame_p[n] *= scale;
        }
        break;
      }
      case TableType::kPulseSweep: {
        // Difference of two sawtooths offset by the pulse width. Pulse
        // width sweeps 0.05 → 0.95 across frames (avoiding extremes so
        // we always have SOME energy).
        const double pw = 0.05 + 0.9 * pos;
        for (uint32_t n = 0; n < kFrameSize; ++n) {
          double x = static_cast<double>(n) / kFrameSize;
          frame_p[n] = x < pw ? 1.0f : -1.0f;
        }
        break;
      }
      case TableType::kBell: {
        // Inharmonic partials (sqrt-based frequencies) with amplitudes
        // that evolve with position. At pos=0 only the fundamental plus
        // a weak overtone; at pos=1 a dense inharmonic spray.
        constexpr double kPartials[] = {1.0, 2.76, 5.40, 8.93, 13.34};
        double peak = 0.0;
        for (uint32_t n = 0; n < kFrameSize; ++n) {
          double x = static_cast<double>(n) / kFrameSize;
          double y = 0.0;
          for (size_t k = 0; k < sizeof(kPartials) / sizeof(kPartials[0]);
               ++k) {
            double amp = 1.0 / static_cast<double>(k + 1);
            // Higher partials fade in with pos.
            amp *= (k == 0) ? 1.0 : pos;
            y += amp * std::sin(kTwoPi * kPartials[k] * x);
          }
          frame_p[n] = static_cast<float>(y);
          if (std::abs(y) > peak)
            peak = std::abs(y);
        }
        if (peak > 1e-9) {
          float scale = static_cast<float>(1.0 / peak);
          for (uint32_t n = 0; n < kFrameSize; ++n)
            frame_p[n] *= scale;
        }
        break;
      }
      case TableType::kVocal: {
        // Sum of three narrow spectral peaks at "formant" positions that
        // shift with frame index. The frame is built directly in the time
        // domain as a sum of sinusoids near the formant frequencies.
        // Formant 1 sweeps 270 → 700, formant 2: 2290 → 1220, formant 3:
        // 3010 → 2600 (these map roughly to "ee → ah").
        const double f1 = 270.0 + pos * 430.0;
        const double f2 = 2290.0 - pos * 1070.0;
        const double f3 = 3010.0 - pos * 410.0;
        // Reference fundamental for the table: one cycle per frame → f_ref
        // = sample_rate / frame_size = 48000/1024 ≈ 46.875 Hz.
        const double f_ref = static_cast<double>(kSampleRate) / kFrameSize;
        auto synth_partial = [](double freq, double f0) {
          return freq / f0;  // partial index (may be fractional)
        };
        double p1 = synth_partial(f1, f_ref);
        double p2 = synth_partial(f2, f_ref);
        double p3 = synth_partial(f3, f_ref);
        double peak = 0.0;
        for (uint32_t n = 0; n < kFrameSize; ++n) {
          double x = static_cast<double>(n) / kFrameSize;
          double y = std::sin(kTwoPi * p1 * x) +
                     0.7 * std::sin(kTwoPi * p2 * x) +
                     0.5 * std::sin(kTwoPi * p3 * x);
          frame_p[n] = static_cast<float>(y);
          if (std::abs(y) > peak)
            peak = std::abs(y);
        }
        if (peak > 1e-9) {
          float scale = static_cast<float>(1.0 / peak);
          for (uint32_t n = 0; n < kFrameSize; ++n)
            frame_p[n] *= scale;
        }
        break;
      }
    }
  }
}

void WavetableOsc::Process(uint32_t num_samples) {
  out_->resize(num_samples);
  const SignalBuffer* freq_in = GetInput("freq");
  const SignalBuffer* freq_mod = GetInput("freq_mod");
  const SignalBuffer* pos_mod = GetInput("position_mod");
  const double inv_sr = 1.0 / kSampleRate;

  for (uint32_t i = 0; i < num_samples; ++i) {
    double freq = freq_in ? static_cast<double>((*freq_in)[i]) : base_freq_hz_;
    if (freq_mod)
      freq += static_cast<double>((*freq_mod)[i]);
    freq = std::clamp(freq, 0.1, kSampleRate * 0.45);
    phase_ += freq * inv_sr;
    if (phase_ >= 1.0)
      phase_ -= 1.0;

    double pos = base_position_;
    if (pos_mod)
      pos += static_cast<double>((*pos_mod)[i]);
    pos = std::clamp(pos, 0.0, 1.0);
    double frame_f = pos * (kNumFrames - 1);
    uint32_t frame_a = static_cast<uint32_t>(frame_f);
    uint32_t frame_b = frame_a + 1 < kNumFrames ? frame_a + 1 : frame_a;
    double frame_mix = frame_f - frame_a;

    double sample_f = phase_ * kFrameSize;
    uint32_t s_a = static_cast<uint32_t>(sample_f) % kFrameSize;
    uint32_t s_b = (s_a + 1u) % kFrameSize;
    double sample_mix = sample_f - std::floor(sample_f);

    const float* fa = &table_[frame_a * kFrameSize];
    const float* fb = &table_[frame_b * kFrameSize];
    double fa_a = static_cast<double>(fa[s_a]);
    double fa_b = static_cast<double>(fa[s_b]);
    double fb_a = static_cast<double>(fb[s_a]);
    double fb_b = static_cast<double>(fb[s_b]);
    double a = (1.0 - sample_mix) * fa_a + sample_mix * fa_b;
    double b = (1.0 - sample_mix) * fb_a + sample_mix * fb_b;
    double sample = (1.0 - frame_mix) * a + frame_mix * b;

    (*out_)[i] = static_cast<float>(sample);
  }
}

// --- NoiseOsc ---
//
// Paul Kellet's 6-pole pink filter (accurate to ±0.05 dB above 9.2 Hz).
// Six one-pole lowpass filters at logarithmically-spaced time constants
// sum into a staircase that approximates the -3 dB/oct pink slope.
//
// See http://www.firstpr.com.au/dsp/pink-noise/#Filtering for derivation.

namespace {
// Scale factor to bring the pink sum back to roughly unit amplitude.
constexpr double kPinkScale = 0.11;

// Brown noise: leaky integrator of white with empirical scaling.
constexpr double kBrownLeak = 0.998;
constexpr double kBrownScale = 3.5;
constexpr double kBrownGain = 0.02;
}  // namespace

NoiseOsc::NoiseOsc(double tilt, uint32_t seed)
    : tilt_(std::clamp(tilt, 0.0, 1.0)),
      rng_state_(seed == 0 ? 0xDEADBEEFu : seed) {
  out_ = AddOutput("out");
}

void NoiseOsc::Process(uint32_t num_samples) {
  out_->resize(num_samples);
  for (uint32_t i = 0; i < num_samples; ++i) {
    // xorshift32: period 2^32 - 1.
    uint32_t s = rng_state_;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    rng_state_ = s;
    // Map to [-1, 1).
    double white = static_cast<double>(s) * (2.0 / 4294967296.0) - 1.0;

    // Pink via Kellet. These coefficients are the standard published set.
    b0_ = 0.99886 * b0_ + white * 0.0555179;
    b1_ = 0.99332 * b1_ + white * 0.0750759;
    b2_ = 0.96900 * b2_ + white * 0.1538520;
    b3_ = 0.86650 * b3_ + white * 0.3104856;
    b4_ = 0.55000 * b4_ + white * 0.5329522;
    b5_ = -0.7616 * b5_ - white * 0.0168980;
    double pink =
        (b0_ + b1_ + b2_ + b3_ + b4_ + b5_ + b6_ + white * 0.5362) * kPinkScale;
    b6_ = white * 0.115926;

    // Brown via leaky integrator.
    brown_state_ = kBrownLeak * brown_state_ + white * kBrownGain;
    double brown = brown_state_ * kBrownScale;

    // Blend based on tilt.
    double sample;
    if (tilt_ < 0.5) {
      double t = tilt_ * 2.0;  // 0..1 across the white→pink range
      sample = (1.0 - t) * white + t * pink;
    } else {
      double t = (tilt_ - 0.5) * 2.0;  // 0..1 across the pink→brown range
      sample = (1.0 - t) * pink + t * brown;
    }
    (*out_)[i] = static_cast<float>(sample);
  }
}

// ============================================================================
// LEGACY: Vco. See header.
// TODO(trace-to-techno): remove once UI migrates off VcoConfig.
// ============================================================================

Vco::Vco(Waveform waveform, double base_freq_hz)
    : waveform_(waveform), base_freq_hz_(base_freq_hz) {
  out_ = AddOutput("out");
}

void Vco::Process(uint32_t num_samples) {
  out_->resize(num_samples);
  const SignalBuffer* freq_mod = GetInput("freq_mod");
  const double inv_sr = 1.0 / kSampleRate;
  for (uint32_t i = 0; i < num_samples; ++i) {
    double freq = base_freq_hz_;
    if (freq_mod)
      freq += static_cast<double>((*freq_mod)[i]);
    phase_ += freq * inv_sr;
    phase_ -= std::floor(phase_);
    float sample = 0.0f;
    switch (waveform_) {
      case Waveform::kSine:
        sample = static_cast<float>(std::sin(kTwoPi * phase_));
        break;
      case Waveform::kSaw:
        sample = static_cast<float>(2.0 * phase_ - 1.0);
        break;
      case Waveform::kSquare:
        sample = phase_ < 0.5 ? 1.0f : -1.0f;
        break;
    }
    (*out_)[i] = sample;
  }
}

}  // namespace perfetto::trace_processor::sound_synth
