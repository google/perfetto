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

#include "src/trace_processor/sound_synth/modules.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace perfetto::trace_processor::sound_synth {

namespace {
constexpr double kTwoPi = 2.0 * 3.14159265358979323846;
}

// --- Vco ---

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
    // Advance phase.
    phase_ += freq * inv_sr;
    phase_ -= std::floor(phase_);  // Wrap to [0, 1).
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

// --- Vca ---

Vca::Vca(double initial_gain) : initial_gain_(initial_gain) {
  out_ = AddOutput("out");
}

void Vca::Process(uint32_t num_samples) {
  out_->resize(num_samples);
  const SignalBuffer* in = GetInput("in");
  const SignalBuffer* gain = GetInput("gain");
  for (uint32_t i = 0; i < num_samples; ++i) {
    float s = in ? (*in)[i] : 0.0f;
    float g = gain ? (*gain)[i] : static_cast<float>(initial_gain_);
    (*out_)[i] = s * g;
  }
}

// --- Envelope ---

Envelope::Envelope(double attack_ms, double decay_ms, double peak)
    : attack_samples_(attack_ms * 0.001 * kSampleRate),
      decay_samples_(decay_ms * 0.001 * kSampleRate),
      peak_(peak) {
  out_ = AddOutput("out");
}

void Envelope::Process(uint32_t num_samples) {
  out_->resize(num_samples);
  const SignalBuffer* trigger = GetInput("trigger");
  // Per-sample attack/decay rates.
  double attack_rate = attack_samples_ > 0 ? peak_ / attack_samples_ : peak_;
  double decay_rate = decay_samples_ > 0 ? peak_ / decay_samples_ : peak_;
  for (uint32_t i = 0; i < num_samples; ++i) {
    float trig = trigger ? (*trigger)[i] : 0.0f;
    // Detect rising edge (or sustained high for re-trigger).
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

// --- Mixer ---

Mixer::Mixer() {
  out_ = AddOutput("out");
}

void Mixer::Process(uint32_t num_samples) {
  out_->assign(num_samples, 0.0f);
  // Sum all connected inputs. The engine connects them as "in", "in.1", etc.
  // We just iterate all inputs.
  for (int idx = 0;; ++idx) {
    std::string port = idx == 0 ? "in" : "in." + std::to_string(idx);
    const SignalBuffer* in = GetInput(port);
    if (!in)
      break;
    for (uint32_t i = 0; i < num_samples; ++i) {
      (*out_)[i] += (*in)[i];
    }
  }
}

// --- TraceSliceSource ---

TraceSliceSource::TraceSliceSource() {
  out_ = AddOutput("out");
}

void TraceSliceSource::Process(uint32_t /*num_samples*/) {
  // Output buffer is pre-filled by SynthEngine. Nothing to do.
}

}  // namespace perfetto::trace_processor::sound_synth
