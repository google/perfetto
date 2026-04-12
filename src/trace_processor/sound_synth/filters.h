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

#ifndef SRC_TRACE_PROCESSOR_SOUND_SYNTH_FILTERS_H_
#define SRC_TRACE_PROCESSOR_SOUND_SYNTH_FILTERS_H_

#include "src/trace_processor/sound_synth/synth_module.h"

namespace perfetto::trace_processor::sound_synth {

// Huovilainen-style 4-pole (24 dB/oct) lowpass ladder filter.
//
// Four cascaded one-pole lowpass stages with a global negative feedback
// path for resonance. Each stage has an embedded tanh-like saturator
// (modeling the transistor nonlinearity of the original Moog circuit).
// At feedback = 4×resonance, the filter self-oscillates; the saturator
// limits the amplitude so the oscillation stays bounded.
//
// Uses fast_tanh(x) = x / (1 + |x|) as a monotonic, smooth sigmoid in
// place of a true tanh (30× cheaper, sonically indistinguishable at our
// drive levels).
//
// Bass-loss compensation: the input has `0.5 * input` added back into the
// feedback path so increasing resonance doesn't kill the bass response
// (per the Huovilainen paper).
//
// Inputs:
//   "in":         audio signal.
//   "cutoff_mod": optional additive cutoff modulation in Hz.
//   "reso_mod":   optional additive resonance modulation in [0, 1].
// Outputs:
//   "out": filtered audio.
class MoogLadder : public SynthModule {
 public:
  MoogLadder(double base_cutoff_hz, double base_resonance, double drive);
  Type type() const override { return Type::kMoogLadder; }
  void Process(uint32_t num_samples) override;

 private:
  double base_cutoff_hz_;
  double base_resonance_;
  double drive_;

  double s1_ = 0.0, s2_ = 0.0, s3_ = 0.0, s4_ = 0.0;

  SignalBuffer* out_;
};

// Chamberlin state-variable filter. Provides LP/HP/BP/Notch modes at 12
// dB/oct. Linear (no saturation). Fast, versatile, stable to ~sr/6.
//
// Inputs:
//   "in":         audio signal.
//   "cutoff_mod": optional additive cutoff modulation in Hz.
//   "q_mod":      optional additive Q modulation (added to base Q).
// Outputs:
//   "out": filtered audio (mode-selected).
class Svf : public SynthModule {
 public:
  enum class Mode { kLowpass, kHighpass, kBandpass, kNotch };
  Svf(Mode mode, double base_cutoff_hz, double base_q);
  Type type() const override { return Type::kSvf; }
  void Process(uint32_t num_samples) override;

 private:
  Mode mode_;
  double base_cutoff_hz_;
  double base_q_;

  double lp_ = 0.0, bp_ = 0.0;

  SignalBuffer* out_;
};

}  // namespace perfetto::trace_processor::sound_synth

#endif  // SRC_TRACE_PROCESSOR_SOUND_SYNTH_FILTERS_H_
