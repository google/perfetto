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

#ifndef SRC_TRACE_PROCESSOR_SOUND_SYNTH_UTILITY_H_
#define SRC_TRACE_PROCESSOR_SOUND_SYNTH_UTILITY_H_

#include <cstdint>

#include "src/trace_processor/sound_synth/synth_module.h"

namespace perfetto::trace_processor::sound_synth {

// Voltage Controlled Amplifier. Multiplies input by gain.
// Inputs:
//   "in": audio signal.
//   "gain" (optional): gain CV (0..1). If not connected, uses initial_gain.
// Outputs:
//   "out": in * gain.
class Vca : public SynthModule {
 public:
  explicit Vca(double initial_gain);
  Type type() const override { return Type::kVca; }
  void Process(uint32_t num_samples) override;

 private:
  double initial_gain_;
  SignalBuffer* out_;
};

// Sums all connected inputs.
// Inputs:
//   "in", "in.1", "in.2", ... : any number of inputs.
// Outputs:
//   "out": sum of all inputs.
class Mixer : public SynthModule {
 public:
  Mixer();
  Type type() const override { return Type::kMixer; }
  void Process(uint32_t num_samples) override;

 private:
  SignalBuffer* out_;
};

}  // namespace perfetto::trace_processor::sound_synth

#endif  // SRC_TRACE_PROCESSOR_SOUND_SYNTH_UTILITY_H_
