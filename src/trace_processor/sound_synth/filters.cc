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

#include "src/trace_processor/sound_synth/filters.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace perfetto::trace_processor::sound_synth {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMinCutoffHz = 20.0;
constexpr double kMaxMoogCutoffHz = 12000.0;  // stability ceiling
constexpr double kMaxSvfCutoffHz = 8000.0;    // Chamberlin safe zone

// Fast tanh-like sigmoid: monotonic, smooth, asymptotes to ±1.
// Not literally tanh but sonically equivalent at moderate drive levels,
// and ~30× cheaper than std::tanh.
inline double FastTanh(double x) {
  return x / (1.0 + (x < 0 ? -x : x));
}

}  // namespace

// --- MoogLadder ---

MoogLadder::MoogLadder(double base_cutoff_hz,
                       double base_resonance,
                       double drive)
    : base_cutoff_hz_(
          std::clamp(base_cutoff_hz, kMinCutoffHz, kMaxMoogCutoffHz)),
      base_resonance_(std::clamp(base_resonance, 0.0, 1.0)),
      drive_(std::max(0.1, drive)) {
  out_ = AddOutput("out");
}

void MoogLadder::Process(uint32_t num_samples) {
  out_->resize(num_samples);
  const SignalBuffer* in = GetInput("in");
  const SignalBuffer* cutoff_mod = GetInput("cutoff_mod");
  const SignalBuffer* reso_mod = GetInput("reso_mod");
  if (!in) {
    std::fill(out_->begin(), out_->end(), 0.0f);
    return;
  }

  for (uint32_t i = 0; i < num_samples; ++i) {
    double cutoff = base_cutoff_hz_;
    if (cutoff_mod)
      cutoff += static_cast<double>((*cutoff_mod)[i]);
    cutoff = std::clamp(cutoff, kMinCutoffHz, kMaxMoogCutoffHz);

    double reso = base_resonance_;
    if (reso_mod)
      reso += static_cast<double>((*reso_mod)[i]);
    reso = std::clamp(reso, 0.0, 1.0);

    // One-pole coefficient. g ∈ (0, 1).
    double g = 1.0 - std::exp(-2.0 * kPi * cutoff / kSampleRate);

    double x = static_cast<double>((*in)[i]) * drive_;
    // Input + feedback with bass compensation.
    double fb_input = FastTanh(x - 4.0 * reso * (s4_ - 0.5 * x));

    // Four cascaded one-pole stages, each with tanh-saturated state.
    s1_ += g * (fb_input - FastTanh(s1_));
    s2_ += g * (FastTanh(s1_) - FastTanh(s2_));
    s3_ += g * (FastTanh(s2_) - FastTanh(s3_));
    s4_ += g * (FastTanh(s3_) - FastTanh(s4_));

    (*out_)[i] = static_cast<float>(s4_);
  }
}

// --- Svf ---

Svf::Svf(Mode mode, double base_cutoff_hz, double base_q)
    : mode_(mode),
      base_cutoff_hz_(
          std::clamp(base_cutoff_hz, kMinCutoffHz, kMaxSvfCutoffHz)),
      base_q_(std::clamp(base_q, 0.5, 50.0)) {
  out_ = AddOutput("out");
}

void Svf::Process(uint32_t num_samples) {
  out_->resize(num_samples);
  const SignalBuffer* in = GetInput("in");
  const SignalBuffer* cutoff_mod = GetInput("cutoff_mod");
  const SignalBuffer* q_mod = GetInput("q_mod");
  if (!in) {
    std::fill(out_->begin(), out_->end(), 0.0f);
    return;
  }

  for (uint32_t i = 0; i < num_samples; ++i) {
    double cutoff = base_cutoff_hz_;
    if (cutoff_mod)
      cutoff += static_cast<double>((*cutoff_mod)[i]);
    cutoff = std::clamp(cutoff, kMinCutoffHz, kMaxSvfCutoffHz);

    double q = base_q_;
    if (q_mod)
      q += static_cast<double>((*q_mod)[i]);
    q = std::clamp(q, 0.5, 50.0);

    double f = 2.0 * std::sin(kPi * cutoff / kSampleRate);
    double damp = 1.0 / q;

    // Double-iterated Chamberlin SVF (2x internal oversampling for
    // stability up to ~sr/6).
    double input = static_cast<double>((*in)[i]);
    double hp = 0.0;
    for (int k = 0; k < 2; ++k) {
      lp_ += f * bp_;
      hp = input - lp_ - damp * bp_;
      bp_ += f * hp;
    }

    double out_val;
    switch (mode_) {
      case Mode::kLowpass:
        out_val = lp_;
        break;
      case Mode::kHighpass:
        out_val = hp;
        break;
      case Mode::kBandpass:
        out_val = bp_;
        break;
      case Mode::kNotch:
        out_val = hp + lp_;
        break;
    }
    (*out_)[i] = static_cast<float>(out_val);
  }
}

}  // namespace perfetto::trace_processor::sound_synth
