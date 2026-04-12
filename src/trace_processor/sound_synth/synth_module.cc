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

#include "src/trace_processor/sound_synth/synth_module.h"

namespace perfetto::trace_processor::sound_synth {

SynthModule::~SynthModule() = default;

void SynthModule::SetInput(const std::string& port, const SignalBuffer* buf) {
  for (auto& p : inputs_) {
    if (p.name == port) {
      p.buf = buf;
      return;
    }
  }
  inputs_.push_back({port, buf});
}

const SignalBuffer* SynthModule::GetOutput(const std::string& port) const {
  for (const auto& p : outputs_) {
    if (p.name == port)
      return &p.buf;
  }
  return nullptr;
}

const SignalBuffer* SynthModule::GetInput(const std::string& port) const {
  for (const auto& p : inputs_) {
    if (p.name == port)
      return p.buf;
  }
  return nullptr;
}

SignalBuffer* SynthModule::AddOutput(const std::string& port) {
  outputs_.push_back({port, {}});
  return &outputs_.back().buf;
}

}  // namespace perfetto::trace_processor::sound_synth
