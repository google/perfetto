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

#include "src/trace_processor/sound_synth/effects.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace perfetto::trace_processor::sound_synth {

namespace {
constexpr double kMaxDelaySeconds = 2.0;
constexpr double kTwoPi = 2.0 * 3.14159265358979323846;
// Maximum chorus delay depth + mid, in ms. Beyond this we saturate.
constexpr double kMaxChorusDelayMs = 60.0;
}  // namespace

// --- Chorus ---

Chorus::Chorus(double rate_hz,
               double depth_ms,
               double mid_delay_ms,
               double mix,
               uint32_t voices)
    : rate_hz_(std::clamp(rate_hz, 0.01, 10.0)),
      mix_(std::clamp(mix, 0.0, 1.0)),
      voices_(std::clamp(voices, 1u, 8u)) {
  // Clamp delay parameters so depth never reaches the buffer edges.
  mid_delay_ms = std::clamp(mid_delay_ms, 1.0, kMaxChorusDelayMs * 0.6);
  depth_ms = std::clamp(depth_ms, 0.0, kMaxChorusDelayMs - mid_delay_ms - 1.0);
  depth_samples_ = depth_ms * 0.001 * kSampleRate;
  mid_delay_samples_ = mid_delay_ms * 0.001 * kSampleRate;
  // Size the circular buffer to the next power of two above the max
  // possible delay, plus a 4-sample interpolation margin.
  double max_delay_samples = mid_delay_samples_ + depth_samples_ + 4.0;
  uint32_t buf_size = 1;
  while (buf_size < static_cast<uint32_t>(max_delay_samples) + 1u)
    buf_size <<= 1;
  buffer_.assign(buf_size, 0.0f);
  buffer_mask_ = buf_size - 1u;
  out_ = AddOutput("out");
}

void Chorus::Process(uint32_t num_samples) {
  out_->resize(num_samples);
  const SignalBuffer* in = GetInput("in");
  if (!in) {
    std::fill(out_->begin(), out_->end(), 0.0f);
    return;
  }
  const double lfo_inc = rate_hz_ / kSampleRate;
  const double voice_recip = voices_ > 0 ? 1.0 / voices_ : 1.0;
  for (uint32_t i = 0; i < num_samples; ++i) {
    double input = static_cast<double>((*in)[i]);
    buffer_[write_pos_] = static_cast<float>(input);

    // Compute the modulated delay time for each voice and sum the reads.
    double wet = 0.0;
    for (uint32_t v = 0; v < voices_; ++v) {
      double phase = lfo_phase_ + static_cast<double>(v) * voice_recip;
      phase -= std::floor(phase);
      double lfo = std::sin(kTwoPi * phase);  // [-1, 1]
      double delay_t = mid_delay_samples_ + depth_samples_ * lfo;
      // Fractional read position.
      double read_f = static_cast<double>(write_pos_) - delay_t;
      // Wrap; we keep read_f in the positive domain.
      while (read_f < 0)
        read_f += static_cast<double>(buffer_.size());
      auto read_i = static_cast<uint32_t>(read_f) & buffer_mask_;
      auto read_i1 = (read_i + 1u) & buffer_mask_;
      double frac = read_f - std::floor(read_f);
      double a = static_cast<double>(buffer_[read_i]);
      double b = static_cast<double>(buffer_[read_i1]);
      wet += (1.0 - frac) * a + frac * b;
    }
    wet *= voice_recip;

    double mixed = (1.0 - mix_) * input + mix_ * wet;
    (*out_)[i] = static_cast<float>(mixed);

    lfo_phase_ += lfo_inc;
    if (lfo_phase_ >= 1.0)
      lfo_phase_ -= 1.0;
    write_pos_ = (write_pos_ + 1u) & buffer_mask_;
  }
}

// --- Delay ---

Delay::Delay(double time_ms, double feedback, double damping, double mix)
    : feedback_(std::clamp(feedback, 0.0, 0.95)),
      damping_(std::clamp(damping, 0.0, 0.99)),
      mix_(std::clamp(mix, 0.0, 1.0)) {
  double time_s = std::max(0.001, time_ms * 0.001);
  time_s = std::min(time_s, kMaxDelaySeconds);
  delay_samples_ = static_cast<uint32_t>(time_s * kSampleRate);
  if (delay_samples_ < 1)
    delay_samples_ = 1;
  // Round buffer size up to the next power of two for cheap modulo via &.
  uint32_t buf_size = 1;
  while (buf_size < delay_samples_ + 1)
    buf_size <<= 1;
  buffer_.assign(buf_size, 0.0f);
  out_ = AddOutput("out");
}

void Delay::Process(uint32_t num_samples) {
  out_->resize(num_samples);
  const SignalBuffer* in = GetInput("in");
  if (!in) {
    std::fill(out_->begin(), out_->end(), 0.0f);
    return;
  }
  const uint32_t mask = static_cast<uint32_t>(buffer_.size()) - 1u;
  const double dry = 1.0 - mix_;
  for (uint32_t i = 0; i < num_samples; ++i) {
    uint32_t read_pos = (write_pos_ + mask + 1u - delay_samples_) & mask;
    double delayed = static_cast<double>(buffer_[read_pos]);

    // One-pole lowpass in feedback path.
    fb_lp_state_ = (1.0 - damping_) * delayed + damping_ * fb_lp_state_;

    double input = static_cast<double>((*in)[i]);
    buffer_[write_pos_] = static_cast<float>(input + feedback_ * fb_lp_state_);

    (*out_)[i] = static_cast<float>(dry * input + mix_ * delayed);
    write_pos_ = (write_pos_ + 1u) & mask;
  }
}

// --- Waveshaper ---

Waveshaper::Waveshaper(Mode mode, double drive, double mix)
    : mode_(mode),
      drive_(std::max(1.0, drive)),
      mix_(std::clamp(mix, 0.0, 1.0)) {
  // Precompute normalization for soft-tanh so unity input → unity output.
  double t = std::tanh(drive_);
  soft_tanh_norm_ = t > 1e-6 ? 1.0 / t : 1.0;
  out_ = AddOutput("out");
}

namespace {

// Iterative reflective wavefolder: reflects the signal at ±1 until bounded.
inline double Fold(double x) {
  for (int iter = 0; iter < 32; ++iter) {
    if (x > 1.0)
      x = 2.0 - x;
    else if (x < -1.0)
      x = -2.0 - x;
    else
      break;
  }
  return x;
}

}  // namespace

void Waveshaper::Process(uint32_t num_samples) {
  out_->resize(num_samples);
  const SignalBuffer* in = GetInput("in");
  if (!in) {
    std::fill(out_->begin(), out_->end(), 0.0f);
    return;
  }
  const double dry = 1.0 - mix_;
  for (uint32_t i = 0; i < num_samples; ++i) {
    double x = static_cast<double>((*in)[i]);
    double driven = x * drive_;
    double shaped = 0.0;
    switch (mode_) {
      case Mode::kSoftTanh:
        shaped = std::tanh(driven) * soft_tanh_norm_;
        break;
      case Mode::kHardClip:
        shaped = std::clamp(driven, -1.0, 1.0);
        break;
      case Mode::kFold:
        shaped = Fold(driven);
        break;
      case Mode::kAsymmetric:
        // DC-biased tanh. The 0.5 bias pushes the waveshape asymmetric,
        // adding even harmonics. Subtract tanh(0.5) so zero input → zero
        // output.
        shaped = std::tanh(driven + 0.5) - std::tanh(0.5);
        // Re-normalize to approximately unit output for unity input.
        shaped *= soft_tanh_norm_;
        break;
    }
    (*out_)[i] = static_cast<float>(dry * x + mix_ * shaped);
  }
}

}  // namespace perfetto::trace_processor::sound_synth
