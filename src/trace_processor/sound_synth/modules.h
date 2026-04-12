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

#ifndef SRC_TRACE_PROCESSOR_SOUND_SYNTH_MODULES_H_
#define SRC_TRACE_PROCESSOR_SOUND_SYNTH_MODULES_H_

#include <cstdint>
#include <vector>

#include "src/trace_processor/sound_synth/synth_module.h"

namespace perfetto::trace_processor::sound_synth {

// Voltage Controlled Oscillator. Generates a periodic waveform.
// Inputs:
//   "freq_mod" (optional): additive frequency modulation in Hz.
// Outputs:
//   "out": audio signal in [-1, 1].
class Vco : public SynthModule {
 public:
  enum class Waveform { kSine, kSaw, kSquare };
  Vco(Waveform waveform, double base_freq_hz);
  Type type() const override { return Type::kVco; }
  void Process(uint32_t num_samples) override;

 private:
  Waveform waveform_;
  double base_freq_hz_;
  double phase_ = 0.0;
  SignalBuffer* out_;
};

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

// Sums all connected inputs.
// Inputs:
//   "in", "in.0", "in.1", ... : any number of inputs.
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

// Trace slice source. Not a real "processing" module - its output buffer
// is pre-filled by the SynthEngine from trace data before the render pass.
// Outputs:
//   "out": the signal derived from trace slices.
class TraceSliceSource : public SynthModule {
 public:
  TraceSliceSource();
  Type type() const override { return Type::kTraceSliceSource; }
  void Process(uint32_t num_samples) override;

  // Called by SynthEngine to provide the pre-computed signal.
  SignalBuffer* GetOutputBuffer() { return out_; }

 private:
  SignalBuffer* out_;
};

}  // namespace perfetto::trace_processor::sound_synth

#endif  // SRC_TRACE_PROCESSOR_SOUND_SYNTH_MODULES_H_
