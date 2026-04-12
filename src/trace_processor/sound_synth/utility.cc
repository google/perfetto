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

#include "src/trace_processor/sound_synth/utility.h"

#include <cstdint>
#include <string>

namespace perfetto::trace_processor::sound_synth {

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

// --- Mixer ---

Mixer::Mixer() {
  out_ = AddOutput("out");
}

void Mixer::Process(uint32_t num_samples) {
  out_->assign(num_samples, 0.0f);
  // Sum all connected inputs. The engine connects them as "in", "in.1", etc.
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

}  // namespace perfetto::trace_processor::sound_synth
