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

#include "src/trace_processor/sound_synth/modulators.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace perfetto::trace_processor::sound_synth {

namespace {
constexpr double kLfoTwoPi = 2.0 * 3.14159265358979323846;
}  // namespace

// --- Adsr ---
//
// The one-pole exponential "target-ratio" formulation. We want `level` to
// approach a target `T` from its current value, reaching it after `N`
// samples. A plain one-pole `level = base + coeff * level` approaches
// asymptotically — it never actually reaches the target. The trick is to
// aim at an overshoot point `T + ratio` (for attack) or `T - ratio` (for
// decay/release), where `ratio` is small, and transition to the next stage
// when `level` crosses `T`.
//
// Derivation: solve for `coeff` such that after N samples the one-pole has
// moved (1 - ratio/(1+ratio)) = 1/(1+ratio) of the way toward the overshoot:
//   coeff = exp(-ln((1 + ratio) / ratio) / N)
//   base  = (target + ratio) * (1 - coeff)   [for attack, target = 1]
//
// For decay: target = sustain, overshoot = sustain - ratio
//   base  = (sustain - ratio) * (1 - coeff)
// For release: target = 0, overshoot = -ratio
//   base  = -ratio * (1 - coeff)
//
// The attack uses a "large" ratio (~0.3) to give a near-linear curve (music
// perceives fast attacks as linear). Decay/release use a "small" ratio
// (~0.0001) for steep, exponential-sounding tails.

namespace {
constexpr double kAttackRatio = 0.3;
constexpr double kDecayReleaseRatio = 0.0001;
constexpr double kReleaseFloor = 1e-4;
}  // namespace

Adsr::Adsr(double attack_ms, double decay_ms, double sustain, double release_ms)
    : sustain_(std::clamp(sustain, 0.0, 1.0)) {
  double attack_samples = std::max(1.0, attack_ms * 0.001 * kSampleRate);
  double decay_samples = std::max(1.0, decay_ms * 0.001 * kSampleRate);
  double release_samples = std::max(1.0, release_ms * 0.001 * kSampleRate);

  ComputeCoeff(attack_samples, kAttackRatio, &attack_coeff_, &attack_base_);
  attack_base_ *= (1.0 + kAttackRatio);

  ComputeCoeff(decay_samples, kDecayReleaseRatio, &decay_coeff_, &decay_base_);
  decay_base_ *= (sustain_ - kDecayReleaseRatio);

  ComputeCoeff(release_samples, kDecayReleaseRatio, &release_coeff_,
               &release_base_);
  release_base_ *= (-kDecayReleaseRatio);

  out_ = AddOutput("out");
}

void Adsr::ComputeCoeff(double rate_samples,
                        double target_ratio,
                        double* coeff,
                        double* multiplier) {
  // coeff = exp(-ln((1 + r) / r) / N)
  *coeff =
      std::exp(-std::log((1.0 + target_ratio) / target_ratio) / rate_samples);
  // caller multiplies by the (target + overshoot) to get the per-sample base.
  *multiplier = 1.0 - *coeff;
}

void Adsr::Process(uint32_t num_samples) {
  out_->resize(num_samples);
  const SignalBuffer* gate = GetInput("gate");
  for (uint32_t i = 0; i < num_samples; ++i) {
    bool gate_now = gate && (*gate)[i] > 0.5f;
    // Detect edges.
    if (gate_now && !gate_high_) {
      stage_ = Stage::kAttack;
    } else if (!gate_now && gate_high_ && stage_ != Stage::kIdle) {
      stage_ = Stage::kRelease;
    }
    gate_high_ = gate_now;

    switch (stage_) {
      case Stage::kAttack:
        level_ = attack_base_ + level_ * attack_coeff_;
        if (level_ >= 1.0) {
          level_ = 1.0;
          stage_ = Stage::kDecay;
        }
        break;
      case Stage::kDecay:
        level_ = decay_base_ + level_ * decay_coeff_;
        if (level_ <= sustain_) {
          level_ = sustain_;
          stage_ = Stage::kSustain;
        }
        break;
      case Stage::kSustain:
        level_ = sustain_;
        break;
      case Stage::kRelease:
        level_ = release_base_ + level_ * release_coeff_;
        if (level_ <= kReleaseFloor) {
          level_ = 0.0;
          stage_ = Stage::kIdle;
        }
        break;
      case Stage::kIdle:
        level_ = 0.0;
        break;
    }
    (*out_)[i] = static_cast<float>(level_);
  }
}

// --- Lfo ---

Lfo::Lfo(Waveform waveform,
         double rate_hz,
         double depth,
         bool bipolar,
         uint32_t seed)
    : waveform_(waveform),
      rate_hz_(std::max(0.001, rate_hz)),
      depth_(std::clamp(depth, 0.0, 1.0)),
      bipolar_(bipolar),
      rng_state_(seed == 0 ? 0xC0FFEE42u : seed) {
  out_ = AddOutput("out");
}

void Lfo::Process(uint32_t num_samples) {
  out_->resize(num_samples);
  const double dt = rate_hz_ / kSampleRate;
  for (uint32_t i = 0; i < num_samples; ++i) {
    double prev_phase = phase_;
    phase_ += dt;
    bool wrapped = false;
    if (phase_ >= 1.0) {
      phase_ -= 1.0;
      wrapped = true;
    }

    // Bipolar value in [-1, 1] before depth/bipolar scaling.
    double v;
    switch (waveform_) {
      case Waveform::kSine:
        v = std::sin(kLfoTwoPi * phase_);
        break;
      case Waveform::kTriangle:
        // Peak at phase=0, valley at phase=0.5.
        v = 2.0 * std::abs(2.0 * phase_ - 1.0) - 1.0;
        break;
      case Waveform::kSquare:
        v = phase_ < 0.5 ? 1.0 : -1.0;
        break;
      case Waveform::kSawUp:
        v = 2.0 * phase_ - 1.0;
        break;
      case Waveform::kSawDown:
        v = 1.0 - 2.0 * phase_;
        break;
      case Waveform::kSampleAndHold: {
        // On first sample, or on phase wrap, sample a new random value.
        if (!held_value_set_ || wrapped || prev_phase > phase_) {
          uint32_t s = rng_state_;
          s ^= s << 13;
          s ^= s >> 17;
          s ^= s << 5;
          rng_state_ = s;
          held_value_ = static_cast<double>(s) * (2.0 / 4294967296.0) - 1.0;
          held_value_set_ = true;
        }
        v = held_value_;
        break;
      }
    }

    // Map to bipolar [-depth, depth] or unipolar [0, depth].
    double out_val = bipolar_ ? v * depth_ : (v * 0.5 + 0.5) * depth_;
    (*out_)[i] = static_cast<float>(out_val);
  }
}

// ============================================================================
// LEGACY: Envelope. See header.
// TODO(trace-to-techno): remove once UI migrates off EnvelopeConfig.
// ============================================================================

Envelope::Envelope(double attack_ms, double decay_ms, double peak)
    : attack_samples_(attack_ms * 0.001 * kSampleRate),
      decay_samples_(decay_ms * 0.001 * kSampleRate),
      peak_(peak) {
  out_ = AddOutput("out");
}

void Envelope::Process(uint32_t num_samples) {
  out_->resize(num_samples);
  const SignalBuffer* trigger = GetInput("trigger");
  double attack_rate = attack_samples_ > 0 ? peak_ / attack_samples_ : peak_;
  double decay_rate = decay_samples_ > 0 ? peak_ / decay_samples_ : peak_;
  for (uint32_t i = 0; i < num_samples; ++i) {
    float trig = trigger ? (*trigger)[i] : 0.0f;
    bool is_triggered = trig > 0.5f;
    if (is_triggered && !was_triggered_) {
      in_attack_ = true;
      in_decay_ = false;
    }
    was_triggered_ = is_triggered;

    if (in_attack_) {
      level_ += attack_rate;
      if (level_ >= peak_) {
        level_ = peak_;
        in_attack_ = false;
        in_decay_ = true;
      }
    } else if (in_decay_) {
      level_ -= decay_rate;
      if (level_ <= 0.0) {
        level_ = 0.0;
        in_decay_ = false;
      }
    }
    (*out_)[i] = static_cast<float>(level_);
  }
}

}  // namespace perfetto::trace_processor::sound_synth
