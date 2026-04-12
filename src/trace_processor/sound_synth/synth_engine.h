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

#ifndef SRC_TRACE_PROCESSOR_SOUND_SYNTH_SYNTH_ENGINE_H_
#define SRC_TRACE_PROCESSOR_SOUND_SYNTH_SYNTH_ENGINE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/sound_synth/synth_module.h"

namespace perfetto::trace_processor {
class TraceProcessor;
}

namespace perfetto::trace_processor::sound_synth {

class SynthEngine {
 public:
  explicit SynthEngine(TraceProcessor* tp);
  ~SynthEngine();

  // Renders audio from the synth patch config (serialized SynthPatch proto).
  // |start_ts| and |end_ts| are trace timestamps in nanoseconds.
  // If both are 0, uses the full trace range.
  // Returns the WAV file as bytes.
  base::StatusOr<std::vector<uint8_t>> Render(const uint8_t* patch_data,
                                              size_t patch_size,
                                              int64_t start_ts,
                                              int64_t end_ts);

 private:
  struct Wire {
    std::string from_module;
    std::string from_port;
    std::string to_module;
    std::string to_port;
    double scale = 1.0;
    double offset = 0.0;
  };

  base::Status BuildModules(const uint8_t* data, size_t size);
  base::Status PopulateTraceSources(int64_t start_ts,
                                    int64_t end_ts,
                                    uint32_t num_samples);
  base::Status ConnectWires(uint32_t num_samples);
  void TopoSort();
  std::vector<uint8_t> EncodeWav(const float* samples, uint32_t num_samples);

  SynthModule* FindModule(const std::string& id);

  TraceProcessor* tp_;
  std::vector<std::unique_ptr<SynthModule>> modules_;
  std::vector<Wire> wires_;
  // Modules in topological order (sources first, output last).
  std::vector<SynthModule*> processing_order_;
  // Intermediate buffers for wires with scale/offset transforms.
  std::vector<std::unique_ptr<SignalBuffer>> transform_buffers_;
};

}  // namespace perfetto::trace_processor::sound_synth

#endif  // SRC_TRACE_PROCESSOR_SOUND_SYNTH_SYNTH_ENGINE_H_
