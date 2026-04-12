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

#ifndef SRC_TRACE_PROCESSOR_SOUND_SYNTH_EFFECTS_H_
#define SRC_TRACE_PROCESSOR_SOUND_SYNTH_EFFECTS_H_

#include <cstdint>
#include <vector>

#include "src/trace_processor/sound_synth/synth_module.h"

namespace perfetto::trace_processor::sound_synth {

// Memoryless nonlinear transfer-function distortion. Four modes, all
// symmetric except kAsymmetric. No oversampling in v1 — accept some
// aliasing from the generated harmonics. For techno this is often
// desirable ("gritty edge").
//
// Modes:
//   kSoftTanh     — tanh(drive*x)/tanh(drive). Warm, odd harmonics only.
//   kHardClip     — clamp(drive*x, -1, 1). Harsh, many harmonics.
//   kFold         — reflective wavefolder. Dense, evolving spectra.
//   kAsymmetric   — DC-biased tanh. Even + odd harmonics.
//
// Inputs:
//   "in": audio signal.
// Outputs:
//   "out": (1 - mix) * in + mix * shape(drive * in).
// Multi-voice modulated-delay chorus. The classic "ensemble" effect that
// gives Solina strings their lushness and Hammond organs their rotary-
// speaker feel.
//
// Architecture: a single short delay line, read by `voices` (default 3)
// virtual taps each modulated by its own sine LFO phase-offset by 1/voices
// of a cycle. The summed wet signal is mixed with the dry input.
//
// The internal buffer is sized for (mid_delay_ms + depth_ms) and a small
// safety margin. Linear interpolation for fractional delay reads.
//
// Inputs:
//   "in": audio signal.
// Outputs:
//   "out": (1 - mix) * in + mix * sum_of_voices.
class Chorus : public SynthModule {
 public:
  Chorus(double rate_hz,
         double depth_ms,
         double mid_delay_ms,
         double mix,
         uint32_t voices);
  Type type() const override { return Type::kChorus; }
  void Process(uint32_t num_samples) override;

 private:
  double rate_hz_;
  double depth_samples_;
  double mid_delay_samples_;
  double mix_;
  uint32_t voices_;

  std::vector<float> buffer_;
  uint32_t buffer_mask_ = 0;  // buffer size is power of two
  uint32_t write_pos_ = 0;
  double lfo_phase_ = 0.0;

  SignalBuffer* out_;
};

// Feedback delay with damped (lowpass-filtered) feedback loop. The damping
// in the feedback path is what gives dub-techno its characteristic "each
// echo gets darker" quality.
//
// Inputs:
//   "in": audio signal.
// Outputs:
//   "out": (1 - mix) * in + mix * delay_line_read.
class Delay : public SynthModule {
 public:
  Delay(double time_ms, double feedback, double damping, double mix);
  Type type() const override { return Type::kDelay; }
  void Process(uint32_t num_samples) override;

 private:
  double feedback_;
  double damping_;
  double mix_;
  // Circular buffer sized for up to kMaxDelaySeconds. The actual delay
  // time determines the read offset.
  std::vector<float> buffer_;
  uint32_t delay_samples_;
  uint32_t write_pos_ = 0;
  double fb_lp_state_ = 0.0;
  SignalBuffer* out_;
};

class Waveshaper : public SynthModule {
 public:
  enum class Mode { kSoftTanh, kHardClip, kFold, kAsymmetric };
  Waveshaper(Mode mode, double drive, double mix);
  Type type() const override { return Type::kWaveshaper; }
  void Process(uint32_t num_samples) override;

 private:
  Mode mode_;
  double drive_;
  double mix_;
  // For kSoftTanh, precomputed 1/tanh(drive) so unity input → unity output.
  double soft_tanh_norm_;
  SignalBuffer* out_;
};

}  // namespace perfetto::trace_processor::sound_synth

#endif  // SRC_TRACE_PROCESSOR_SOUND_SYNTH_EFFECTS_H_
