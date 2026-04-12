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

#ifndef SRC_TRACE_PROCESSOR_SOUND_SYNTH_MODULATORS_H_
#define SRC_TRACE_PROCESSOR_SOUND_SYNTH_MODULATORS_H_

#include <cstdint>

#include "src/trace_processor/sound_synth/synth_module.h"

namespace perfetto::trace_processor::sound_synth {

// 4-stage exponential envelope generator (Attack/Decay/Sustain/Release).
//
// Each stage uses a one-pole filter that approaches an overshoot target, so
// the curves are naturally exponential (attack curves toward 1+alpha, decay
// toward sustain-beta, release toward 0-beta). Attack uses a relatively
// relaxed target ratio (large alpha) → near-linear feel; decay/release use
// a tight target ratio → steep, natural-sounding exponential tails.
//
// Gate semantics:
//   rising edge (gate > 0.5, previously <= 0.5): → ATTACK
//   falling edge (gate <= 0.5, previously > 0.5): → RELEASE
// Re-triggering from any state picks up from the current level (click-free).
//
// Inputs:
//   "gate": gate signal (audio-rate, interpreted as 0/1 with threshold 0.5).
// Outputs:
//   "out": envelope level in [0, 1].
class Adsr : public SynthModule {
 public:
  Adsr(double attack_ms, double decay_ms, double sustain, double release_ms);
  Type type() const override { return Type::kAdsr; }
  void Process(uint32_t num_samples) override;

 private:
  enum class Stage { kIdle, kAttack, kDecay, kSustain, kRelease };

  // Coefficient/base pair for a one-pole exponential targeting an overshoot
  // point. See the implementation for the derivation.
  static void ComputeCoeff(double rate_samples,
                           double target_ratio,
                           double* coeff,
                           double* multiplier);

  double sustain_;
  double attack_coeff_ = 0.0;
  double attack_base_ = 0.0;
  double decay_coeff_ = 0.0;
  double decay_base_ = 0.0;
  double release_coeff_ = 0.0;
  double release_base_ = 0.0;

  Stage stage_ = Stage::kIdle;
  double level_ = 0.0;
  bool gate_high_ = false;

  SignalBuffer* out_;
};

// Low-frequency oscillator. Produces a control signal, not audio — intended
// for modulating parameters (filter cutoff, amplitude, etc.). Runs at the
// same sample rate as the audio engine so we can wire it into any input
// port uniformly; the high rate is wasted on slow modulation but keeps the
// graph model simple.
//
// Waveforms:
//   kSine, kTriangle, kSquare, kSawUp, kSawDown — deterministic periodic.
//   kSampleAndHold — emits a new uniform random value each time the phase
//                    wraps, held constant between wraps.
//
// Output range:
//   bipolar=true  → [-depth, +depth]   (default, good for filter sweeps)
//   bipolar=false → [0, depth]         (good for amplitude / cutoff base)
//
// No input ports. Runs free.
// Outputs:
//   "out": modulation signal.
class Lfo : public SynthModule {
 public:
  enum class Waveform {
    kSine,
    kTriangle,
    kSquare,
    kSawUp,
    kSawDown,
    kSampleAndHold,
  };
  Lfo(Waveform waveform,
      double rate_hz,
      double depth,
      bool bipolar,
      uint32_t seed);
  Type type() const override { return Type::kLfo; }
  void Process(uint32_t num_samples) override;

 private:
  Waveform waveform_;
  double rate_hz_;
  double depth_;
  bool bipolar_;

  double phase_ = 0.0;
  double held_value_ = 0.0;
  bool held_value_set_ = false;
  uint32_t rng_state_;

  SignalBuffer* out_;
};

// ============================================================================
// LEGACY: the old Attack-Decay-only Envelope is kept so existing UI patches
// referencing `EnvelopeConfig` in the proto keep working. New patches should
// prefer the new Adsr above, which is a proper 4-stage exponential envelope.
// TODO(trace-to-techno): remove once UI migrates off EnvelopeConfig.
// ============================================================================

// Attack-Decay envelope generator.
// Inputs:
//   "trigger": gate/trigger signal. Rising edge starts the envelope.
// Outputs:
//   "out": envelope value (0..peak).
class Envelope : public SynthModule {
 public:
  Envelope(double attack_ms, double decay_ms, double peak);
  Type type() const override { return Type::kEnvelope; }
  void Process(uint32_t num_samples) override;

 private:
  double attack_samples_;
  double decay_samples_;
  double peak_;
  double level_ = 0.0;
  bool in_attack_ = false;
  bool in_decay_ = false;
  bool was_triggered_ = false;
  SignalBuffer* out_;
};

}  // namespace perfetto::trace_processor::sound_synth

#endif  // SRC_TRACE_PROCESSOR_SOUND_SYNTH_MODULATORS_H_
