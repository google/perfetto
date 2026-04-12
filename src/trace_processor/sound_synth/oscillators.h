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

#ifndef SRC_TRACE_PROCESSOR_SOUND_SYNTH_OSCILLATORS_H_
#define SRC_TRACE_PROCESSOR_SOUND_SYNTH_OSCILLATORS_H_

#include <cstdint>
#include <vector>

#include "src/trace_processor/sound_synth/synth_module.h"

namespace perfetto::trace_processor::sound_synth {

// Bandlimited classic virtual-analog oscillator. Anti-aliased saw, square
// (with PWM), triangle, and sine waveforms.
//
// Aliasing suppression:
//   saw/square: polyBLEP correction at each phase discontinuity (~2-sample
//               polynomial residual, ~40-50 dB of alias suppression).
//   triangle:   integrate an anti-aliased square with a leaky integrator.
//   sine:       none needed (pure sine has no harmonics).
//
// Inputs:
//   "freq"     (optional, absolute Hz): when connected, replaces base_freq.
//   "freq_mod" (optional, additive Hz): added on top of freq/base_freq.
//   "reset"    (optional, rising-edge gate): resets phase to 0 at sample
//              start (essential for kick drums).
// Outputs:
//   "out": audio signal in [-1, 1].
class ClassicOsc : public SynthModule {
 public:
  enum class Waveform { kSaw, kSquare, kTriangle, kSine };
  ClassicOsc(Waveform waveform, double base_freq_hz, double pulse_width);
  Type type() const override { return Type::kClassicOsc; }
  void Process(uint32_t num_samples) override;

 private:
  Waveform waveform_;
  double base_freq_hz_;
  double pulse_width_;

  double phase_ = 0.0;
  bool reset_prev_high_ = false;

  SignalBuffer* out_;
};

// 2-operator phase-modulation FM oscillator (Chowning). One sine modulator
// modulates the phase of one sine carrier. Modulator has optional self-
// feedback which at high levels produces saw-like character (DX7 trick).
//
// Inputs:
//   "freq" / "freq_mod": carrier frequency in Hz.
//   "index_mod": additive modulation-index.
// Outputs:
//   "out": audio signal.
class FmOsc : public SynthModule {
 public:
  FmOsc(double base_freq_hz,
        double mod_ratio,
        double mod_index,
        double feedback);
  Type type() const override { return Type::kFmOsc; }
  void Process(uint32_t num_samples) override;

 private:
  double base_freq_hz_;
  double mod_ratio_;
  double mod_index_;
  double feedback_;

  double car_phase_ = 0.0;
  double mod_phase_ = 0.0;
  double last_mod_ = 0.0;
  SignalBuffer* out_;
};

// Phase-distortion oscillator (Casio CZ style). A linear phase ramp is
// warped through a nonlinear function before being fed into sin(), giving
// filter-sweep-like timbral morphs without a filter.
//
// Modes:
//   kSawWarp:   compresses the first half of the cycle. amount=0 is sine,
//               amount=1 approaches a sawtooth shape.
//   kPulseWarp: compresses both transitions, leaving flat top/bottom.
//               amount=0 is sine, amount=1 approaches a pulse.
//
// Inputs:
//   "freq" / "freq_mod": carrier Hz.
//   "amount_mod": additive amount in [0, 1].
// Outputs:
//   "out": audio signal.
class PhaseDistortionOsc : public SynthModule {
 public:
  enum class Mode { kSawWarp, kPulseWarp };
  PhaseDistortionOsc(Mode mode, double base_freq_hz, double amount);
  Type type() const override { return Type::kPhaseDistortionOsc; }
  void Process(uint32_t num_samples) override;

 private:
  Mode mode_;
  double base_freq_hz_;
  double amount_;
  double phase_ = 0.0;
  SignalBuffer* out_;
};

// Wavefolder oscillator (West Coast / Massive X "Gorilla" flavor). Uses
// the smooth sin(drive*sin(2π*phase)) form — no fold creases, no fold
// aliasing, just musical harmonic explosion as drive rises.
//
// Inputs:
//   "freq" / "freq_mod": carrier Hz.
//   "drive_mod": additive drive.
// Outputs:
//   "out": audio signal.
class FoldOsc : public SynthModule {
 public:
  FoldOsc(double base_freq_hz, double drive, double bias);
  Type type() const override { return Type::kFoldOsc; }
  void Process(uint32_t num_samples) override;

 private:
  double base_freq_hz_;
  double drive_;
  double bias_;
  double phase_ = 0.0;
  SignalBuffer* out_;
};

// Hardsync oscillator. Two independent phase accumulators (master and
// slave). The slave runs at master_freq * sync_ratio. On every master
// cycle completion, the slave's phase is forcibly reset to 0 — the
// distinctive "sync sweep" sound emerges when sync_ratio is modulated.
//
// Slave waveform is polyBLEP sawtooth. Sync-reset aliasing is present and
// accepted as part of the character in v1.
//
// Inputs:
//   "freq" / "freq_mod": master frequency in Hz.
//   "ratio_mod": additive sync ratio.
// Outputs:
//   "out": audio signal.
class SyncOsc : public SynthModule {
 public:
  SyncOsc(double base_freq_hz, double sync_ratio);
  Type type() const override { return Type::kSyncOsc; }
  void Process(uint32_t num_samples) override;

 private:
  double base_freq_hz_;
  double sync_ratio_;
  double master_phase_ = 0.0;
  double slave_phase_ = 0.0;
  SignalBuffer* out_;
};

// Supersaw (Roland JP-8000 style). 7 detuned polyBLEP sawtooths stacked
// together: 1 center + 3 pairs of detuned sides. Per Adam Szabo's reverse-
// engineering, detune spread is non-linear; here we use a simple
// exponential mapping good enough for techno.
//
// Inputs:
//   "freq" / "freq_mod": center frequency in Hz.
// Outputs:
//   "out": audio signal.
class SuperOsc : public SynthModule {
 public:
  SuperOsc(double base_freq_hz, double detune, double mix);
  Type type() const override { return Type::kSuperOsc; }
  void Process(uint32_t num_samples) override;

 private:
  double base_freq_hz_;
  double detune_;
  double mix_;
  // Fixed initial phases for reproducibility across runs.
  double phases_[7] = {0.0, 0.143, 0.286, 0.429, 0.572, 0.715, 0.858};
  SignalBuffer* out_;
};

// Hammond B3-style drawbar organ: additive synthesis of 9 sine partials
// at the classic drawbar ratios, normalized so "full drawbars" ≈ unit
// amplitude.
//
// Inputs:
//   "freq" / "freq_mod": fundamental frequency in Hz.
// Outputs:
//   "out": audio signal.
class DrawbarOrgan : public SynthModule {
 public:
  // Drawbar levels are in the order 16', 5⅓', 8', 4', 2⅔', 2', 1⅗',
  // 1⅓', 1' — matching the Hammond console left-to-right layout.
  DrawbarOrgan(double base_freq_hz, const double levels[9]);
  Type type() const override { return Type::kDrawbarOrgan; }
  void Process(uint32_t num_samples) override;

 private:
  static constexpr uint32_t kNumDrawbars = 9;
  // Ratios for the 9 drawbars relative to the played fundamental.
  static constexpr double kRatios[kNumDrawbars] = {
      0.5,  // 16'    sub octave
      1.5,  // 5⅓'    perfect 5th
      1.0,  // 8'     fundamental
      2.0,  // 4'     octave
      3.0,  // 2⅔'    12th
      4.0,  // 2'     two octaves
      5.0,  // 1⅗'    17th
      6.0,  // 1⅓'    19th
      8.0,  // 1'     three octaves
  };
  double base_freq_hz_;
  double levels_[kNumDrawbars];
  double level_sum_;
  double phases_[kNumDrawbars] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  SignalBuffer* out_;
};

// Procedural wavetable oscillator. 32 frames × 1024 samples, constructed
// at build time from a formula selected by `TableType`. A `position`
// parameter (0..1) scans through the frames; linear interpolation between
// adjacent frames and adjacent samples within a frame.
//
// No mipmap / band-limiting in v1 — harmonic content is inherently limited
// by the table generation formulas. Above ~4 kHz fundamental there will be
// some aliasing (typical techno use is well below this).
//
// Table types:
//   kSineToSaw  — pure sine at pos=0, sawtooth at pos=1 (additive harmonic
//                 build-up).
//   kPulseSweep — pulse wave with PW sweeping 5% → 95% across frames.
//   kBell       — inharmonic FM-style bell timbres morphing with position.
//   kVocal      — sum of three formant peaks whose frequencies shift with
//                 position, giving an "ah → ee → oo" morph.
//
// Inputs:
//   "freq"         (optional, abs Hz) / "freq_mod" (optional, additive Hz)
//   "position_mod" (optional, additive 0..1, clamped)
// Outputs:
//   "out": audio signal in [-1, 1].
class WavetableOsc : public SynthModule {
 public:
  enum class TableType { kSineToSaw, kPulseSweep, kBell, kVocal };
  WavetableOsc(TableType table_type, double base_freq_hz, double base_position);
  Type type() const override { return Type::kWavetableOsc; }
  void Process(uint32_t num_samples) override;

 private:
  static constexpr uint32_t kNumFrames = 32;
  static constexpr uint32_t kFrameSize = 1024;

  void BuildTable();

  TableType table_type_;
  double base_freq_hz_;
  double base_position_;
  std::vector<float> table_;  // kNumFrames * kFrameSize samples
  double phase_ = 0.0;

  SignalBuffer* out_;
};

// Colored noise generator with a continuous tilt parameter.
//
//   tilt = 0.0  → pure white noise (flat spectrum)
//   tilt = 0.5  → pink noise (-3 dB/oct, via Paul Kellet's 6-pole filter)
//   tilt = 1.0  → brown noise (-6 dB/oct, via leaky integrator)
//
// Intermediate values linearly blend between the two adjacent spectra. The
// pink and brown state are ALWAYS updated (regardless of tilt) so the
// spectral "personality" is consistent as tilt is modulated.
//
// The PRNG is xorshift32 with a configurable seed (reproducible across
// runs given the same seed, which matters for the trace→WAV pipeline).
//
// Outputs:
//   "out": noise sample, nominally in [-1, 1].
class NoiseOsc : public SynthModule {
 public:
  NoiseOsc(double tilt, uint32_t seed);
  Type type() const override { return Type::kNoiseOsc; }
  void Process(uint32_t num_samples) override;

 private:
  double tilt_;

  // xorshift32 state.
  uint32_t rng_state_;

  // Kellet pink filter state (6 one-pole filters running in parallel).
  double b0_ = 0.0, b1_ = 0.0, b2_ = 0.0, b3_ = 0.0, b4_ = 0.0, b5_ = 0.0,
         b6_ = 0.0;

  // Brown noise state (leaky integrator of white).
  double brown_state_ = 0.0;

  SignalBuffer* out_;
};

// ============================================================================
// LEGACY: the old naive Vco is kept so existing UI patches referencing
// `VcoConfig` in the proto keep working. New patches should prefer the new
// ClassicOsc above, which is bandlimited via polyBLEP.
// TODO(trace-to-techno): remove once UI migrates off VcoConfig.
// ============================================================================

// Voltage Controlled Oscillator. Generates a periodic waveform.
// Inputs:
//   "freq_mod" (optional): additive frequency modulation in Hz.
// Outputs:
//   "out": audio signal in [-1, 1].
class Vco : public SynthModule {
 public:
  enum class Waveform { kSine, kSaw, kSquare };
  Vco(Waveform waveform, double base_freq_hz);
  Type type() const override { return Type::kVco; }
  void Process(uint32_t num_samples) override;

 private:
  Waveform waveform_;
  double base_freq_hz_;
  double phase_ = 0.0;
  SignalBuffer* out_;
};

}  // namespace perfetto::trace_processor::sound_synth

#endif  // SRC_TRACE_PROCESSOR_SOUND_SYNTH_OSCILLATORS_H_
