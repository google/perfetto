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

#ifndef SRC_TRACE_PROCESSOR_SOUND_SYNTH_SYNTH_MODULE_H_
#define SRC_TRACE_PROCESSOR_SOUND_SYNTH_SYNTH_MODULE_H_

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace perfetto::trace_processor::sound_synth {

// Sample rate used for all audio rendering.
inline constexpr uint32_t kSampleRate = 48000;

// Time dilation factor: trace time is stretched by this amount.
// Rationale: 120 FPS (typical Android trace cadence) mapped to 150 BPM
// (typical techno tempo) gives 120 / 2.5 = 48x.
// So 1 second of trace becomes 48 seconds of audio.
inline constexpr double kTimeDilation = 48.0;

// A buffer of audio/CV samples. All signals in the synth graph are represented
// as vectors of floats at kSampleRate.
using SignalBuffer = std::vector<float>;

// Base class for all synth modules. Each module has named input and output
// ports. The engine calls Process() once per render pass after all inputs
// have been connected.
class SynthModule {
 public:
  enum class Type {
    kTraceSliceSource,
    kTraceCounterSource,
    kTestPatternSource,
    kVco,
    kVca,
    kEnvelope,
    kMixer,
    // New generation of blocks (tasks #2-#15). New types land here.
    kAdsr,
    kClassicOsc,
    kNoiseOsc,
    kLfo,
    kWaveshaper,
    kMoogLadder,
    kSvf,
    kDelay,
    kWavetableOsc,
    kFmOsc,
    kPhaseDistortionOsc,
    kFoldOsc,
    kSyncOsc,
    kSuperOsc,
    // Batch 2: analog-strings / substance / organ support.
    kChorus,
    kDrawbarOrgan,
  };

  virtual ~SynthModule();

  virtual Type type() const = 0;

  // Returns the module's unique ID (from the patch config).
  const std::string& id() const { return id_; }
  void set_id(const std::string& id) { id_ = id; }

  // Called by the engine before Process() to set an input port's buffer.
  // The buffer is owned by whoever produces it (the source module or the
  // engine for trace sources). The pointer is valid for the duration of
  // the render pass.
  void SetInput(const std::string& port, const SignalBuffer* buf);

  // Renders |num_samples| into the output buffer(s). Subclasses override.
  virtual void Process(uint32_t num_samples) = 0;

  // Returns the output buffer for the given port name.
  // Most modules have a single "out" port.
  const SignalBuffer* GetOutput(const std::string& port) const;

 protected:
  // Access an input buffer by port name. Returns nullptr if not connected.
  const SignalBuffer* GetInput(const std::string& port) const;

  // Subclasses call this to register their output port and get a pointer
  // to the buffer they should write into.
  SignalBuffer* AddOutput(const std::string& port);

 private:
  std::string id_;

  struct Port {
    std::string name;
    const SignalBuffer* buf = nullptr;
  };
  std::vector<Port> inputs_;

  struct OutputPort {
    std::string name;
    SignalBuffer buf;
  };
  // std::deque rather than vector so that the pointers returned by
  // AddOutput() remain valid when subclasses register multiple ports.
  std::deque<OutputPort> outputs_;
};

}  // namespace perfetto::trace_processor::sound_synth

#endif  // SRC_TRACE_PROCESSOR_SOUND_SYNTH_SYNTH_MODULE_H_
