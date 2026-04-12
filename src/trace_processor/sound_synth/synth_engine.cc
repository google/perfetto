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

#include "src/trace_processor/sound_synth/synth_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/sound_synth/modules.h"
#include "src/trace_processor/sound_synth/synth_module.h"

#include "protos/perfetto/trace_processor/synth.pbzero.h"

namespace perfetto::trace_processor::sound_synth {

namespace {

using SynthPatchProto = protos::pbzero::SynthPatch;
using SynthModuleProto = protos::pbzero::SynthModule;
using WireProto = protos::pbzero::SynthWire;
using SliceSourceProto = protos::pbzero::TraceSliceSourceConfig;
using VcoProto = protos::pbzero::VcoConfig;
using VcaProto = protos::pbzero::VcaConfig;
using EnvProto = protos::pbzero::EnvelopeConfig;

}  // namespace

SynthEngine::SynthEngine(TraceProcessor* tp) : tp_(tp) {}
SynthEngine::~SynthEngine() = default;

base::StatusOr<std::vector<uint8_t>> SynthEngine::Render(
    const uint8_t* patch_data,
    size_t patch_size,
    int64_t start_ts,
    int64_t end_ts) {
  modules_.clear();
  wires_.clear();
  processing_order_.clear();
  transform_buffers_.clear();

  // If timestamps are not provided, query the trace range.
  if (start_ts == 0 && end_ts == 0) {
    auto it = tp_->ExecuteQuery(
        "SELECT min(ts), max(ts + dur) FROM slice WHERE dur > 0");
    if (it.Next()) {
      if (it.Get(0).type != SqlValue::kNull) {
        start_ts = it.Get(0).AsLong();
        end_ts = it.Get(1).AsLong();
      }
    }
    auto status = it.Status();
    if (!status.ok())
      return status;
    if (start_ts == 0 && end_ts == 0)
      return base::ErrStatus("No slice data in trace");
  }

  // Compute number of samples. Apply time dilation so trace events
  // map to musical tempo (120 FPS -> 150 BPM = 48x stretch).
  double trace_duration_secs =
      static_cast<double>(end_ts - start_ts) / 1000000000.0;
  double duration_secs = trace_duration_secs * kTimeDilation;
  if (duration_secs <= 0)
    return base::ErrStatus("Invalid time range");
  auto num_samples = static_cast<uint32_t>(duration_secs * kSampleRate);
  if (num_samples == 0)
    return base::ErrStatus("Time range too short");

  // 1. Parse patch config and create modules.
  RETURN_IF_ERROR(BuildModules(patch_data, patch_size));

  // 2. Fill trace source buffers from trace data.
  RETURN_IF_ERROR(PopulateTraceSources(start_ts, end_ts, num_samples));

  // 3. Determine processing order (topological sort).
  TopoSort();

  // 4. Connect wires and process.
  RETURN_IF_ERROR(ConnectWires(num_samples));

  // 5. Process all modules in order.
  for (auto* mod : processing_order_) {
    mod->Process(num_samples);
  }

  // 6. Find the "master" mixer or last module's output.
  SynthModule* output_mod = FindModule("master");
  if (!output_mod && !modules_.empty())
    output_mod = processing_order_.back();
  if (!output_mod)
    return base::ErrStatus("No modules in patch");

  const SignalBuffer* out = output_mod->GetOutput("out");
  if (!out || out->empty())
    return base::ErrStatus("Output module produced no audio");

  return EncodeWav(out->data(), static_cast<uint32_t>(out->size()));
}

base::Status SynthEngine::BuildModules(const uint8_t* data, size_t size) {
  SynthPatchProto::Decoder patch(data, size);

  for (auto it = patch.modules(); it; ++it) {
    SynthModuleProto::Decoder mod(*it);
    std::string id = mod.id().ToStdString();
    if (id.empty())
      return base::ErrStatus("Module missing id");

    std::unique_ptr<SynthModule> m;
    if (mod.has_trace_slice_source()) {
      m = std::make_unique<TraceSliceSource>();
    } else if (mod.has_vco()) {
      VcoProto::Decoder cfg(mod.vco());
      Vco::Waveform wf = Vco::Waveform::kSine;
      if (cfg.has_waveform()) {
        switch (cfg.waveform()) {
          case VcoProto::SAW:
            wf = Vco::Waveform::kSaw;
            break;
          case VcoProto::SQUARE:
            wf = Vco::Waveform::kSquare;
            break;
          default:
            break;
        }
      }
      double freq = cfg.has_base_freq_hz() ? cfg.base_freq_hz() : 440.0;
      m = std::make_unique<Vco>(wf, freq);
    } else if (mod.has_vca()) {
      VcaProto::Decoder cfg(mod.vca());
      double gain = cfg.has_initial_gain() ? cfg.initial_gain() : 1.0;
      m = std::make_unique<Vca>(gain);
    } else if (mod.has_envelope()) {
      EnvProto::Decoder cfg(mod.envelope());
      double attack = cfg.has_attack_ms() ? cfg.attack_ms() : 5.0;
      double decay = cfg.has_decay_ms() ? cfg.decay_ms() : 200.0;
      double peak = cfg.has_peak() ? cfg.peak() : 1.0;
      m = std::make_unique<Envelope>(attack, decay, peak);
    } else if (mod.has_mixer()) {
      m = std::make_unique<Mixer>();
    } else {
      return base::ErrStatus("Module '%s' has unknown type", id.c_str());
    }

    m->set_id(id);
    modules_.push_back(std::move(m));
  }

  // Parse wires.
  for (auto it = patch.wires(); it; ++it) {
    WireProto::Decoder w(*it);
    Wire wire;
    wire.from_module = w.from_module().ToStdString();
    wire.from_port = w.has_from_port() ? w.from_port().ToStdString() : "out";
    wire.to_module = w.to_module().ToStdString();
    wire.to_port = w.has_to_port() ? w.to_port().ToStdString() : "in";
    wire.scale = w.has_scale() ? w.scale() : 1.0;
    wire.offset = w.has_offset() ? w.offset() : 0.0;
    wires_.push_back(std::move(wire));
  }

  return base::OkStatus();
}

base::Status SynthEngine::PopulateTraceSources(int64_t start_ts,
                                               int64_t end_ts,
                                               uint32_t num_samples) {
  double ns_per_sample =
      static_cast<double>(end_ts - start_ts) / num_samples;

  for (auto& mod : modules_) {
    if (mod->type() != SynthModule::Type::kTraceSliceSource)
      continue;
    auto* src = static_cast<TraceSliceSource*>(mod.get());

    // Find the config for this trace source from the patch.
    // We need to re-parse; store config alongside? For now, re-parse is fine
    // since this is called once.

    SignalBuffer* buf = src->GetOutputBuffer();
    buf->assign(num_samples, 0.0f);

    // Find the slice source config. We stored only the module type, but
    // we need the track_name_glob. For this first version, we query ALL
    // slices and let the gate signal be 1 when any slice is active.
    // TODO: filter by track_name_glob from the config.

    std::string sql =
        "SELECT s.ts, s.dur FROM slice s "
        "WHERE s.dur > 0 AND s.ts >= " +
        std::to_string(start_ts) + " AND s.ts < " + std::to_string(end_ts) +
        " ORDER BY s.ts";

    auto it = tp_->ExecuteQuery(sql);
    while (it.Next()) {
      int64_t ts = it.Get(0).AsLong();
      int64_t dur = it.Get(1).AsLong();
      // Convert to sample indices.
      auto s_start = static_cast<int64_t>(
          static_cast<double>(ts - start_ts) / ns_per_sample);
      auto s_end = static_cast<int64_t>(
          static_cast<double>(ts + dur - start_ts) / ns_per_sample);
      s_start = std::max(s_start, int64_t{0});
      s_end = std::min(s_end, static_cast<int64_t>(num_samples));
      // For GATE signal: set to 1.0 while slice is active.
      for (int64_t s = s_start; s < s_end; ++s) {
        (*buf)[static_cast<size_t>(s)] = 1.0f;
      }
    }
    RETURN_IF_ERROR(it.Status());
  }
  return base::OkStatus();
}

base::Status SynthEngine::ConnectWires(uint32_t num_samples) {
  // Track how many inputs each mixer port has received (for auto-naming).
  std::map<std::string, int> mixer_input_counts;

  for (const auto& wire : wires_) {
    SynthModule* from = FindModule(wire.from_module);
    SynthModule* to = FindModule(wire.to_module);
    if (!from)
      return base::ErrStatus("Wire references unknown module '%s'",
                             wire.from_module.c_str());
    if (!to)
      return base::ErrStatus("Wire references unknown module '%s'",
                             wire.to_module.c_str());

    const SignalBuffer* src_buf = from->GetOutput(wire.from_port);
    if (!src_buf)
      return base::ErrStatus("Module '%s' has no output port '%s'",
                             wire.from_module.c_str(),
                             wire.from_port.c_str());

    // If scale/offset are non-trivial, create an intermediate buffer.
    const SignalBuffer* connect_buf = src_buf;
    if (wire.scale != 1.0 || wire.offset != 0.0) {
      auto transformed = std::make_unique<SignalBuffer>(num_samples);
      for (uint32_t i = 0; i < num_samples; ++i) {
        (*transformed)[i] =
            static_cast<float>(static_cast<double>((*src_buf)[i]) *
                               wire.scale + wire.offset);
      }
      connect_buf = transformed.get();
      transform_buffers_.push_back(std::move(transformed));
    }

    // For mixer modules, auto-increment the input port name.
    std::string to_port = wire.to_port;
    if (to->type() == SynthModule::Type::kMixer && to_port == "in") {
      int& count = mixer_input_counts[wire.to_module];
      if (count > 0)
        to_port = "in." + std::to_string(count);
      count++;
    }

    to->SetInput(to_port, connect_buf);
  }
  return base::OkStatus();
}

void SynthEngine::TopoSort() {
  // Simple topological sort based on wire dependencies.
  // Build adjacency: for each module, which modules must come before it.
  std::map<std::string, std::vector<std::string>> deps;
  for (const auto& mod : modules_)
    deps[mod->id()];  // Ensure all modules are in the map.
  for (const auto& wire : wires_)
    deps[wire.to_module].push_back(wire.from_module);

  std::set<std::string> visited;
  std::set<std::string> in_stack;
  processing_order_.clear();

  // DFS-based topo sort.
  std::function<void(const std::string&)> visit =
      [&](const std::string& id) {
        if (visited.count(id))
          return;
        in_stack.insert(id);
        visited.insert(id);
        for (const auto& dep : deps[id]) {
          if (!in_stack.count(dep))
            visit(dep);
        }
        in_stack.erase(id);
        SynthModule* m = FindModule(id);
        if (m)
          processing_order_.push_back(m);
      };

  for (const auto& mod : modules_)
    visit(mod->id());
}

SynthModule* SynthEngine::FindModule(const std::string& id) {
  for (auto& m : modules_) {
    if (m->id() == id)
      return m.get();
  }
  return nullptr;
}

std::vector<uint8_t> SynthEngine::EncodeWav(const float* samples,
                                            uint32_t num_samples) {
  // WAV format: 32-bit float, mono, 48kHz.
  constexpr uint32_t kNumChannels = 1;
  constexpr uint32_t kBitsPerSample = 32;
  constexpr uint32_t kBytesPerSample = kBitsPerSample / 8;
  uint32_t data_size = num_samples * kNumChannels * kBytesPerSample;
  uint32_t file_size = 44 + data_size;

  std::vector<uint8_t> wav(file_size);
  uint8_t* p = wav.data();

  auto write_u32 = [](uint8_t* dst, uint32_t val) {
    memcpy(dst, &val, 4);
  };
  auto write_u16 = [](uint8_t* dst, uint16_t val) {
    memcpy(dst, &val, 2);
  };

  // RIFF header.
  memcpy(p, "RIFF", 4);
  p += 4;
  write_u32(p, file_size - 8);
  p += 4;
  memcpy(p, "WAVE", 4);
  p += 4;

  // fmt chunk.
  memcpy(p, "fmt ", 4);
  p += 4;
  write_u32(p, 16);  // Chunk size.
  p += 4;
  write_u16(p, 3);  // Audio format: IEEE float.
  p += 2;
  write_u16(p, static_cast<uint16_t>(kNumChannels));
  p += 2;
  write_u32(p, kSampleRate);
  p += 4;
  write_u32(p, kSampleRate * kNumChannels * kBytesPerSample);  // Byte rate.
  p += 4;
  write_u16(p, static_cast<uint16_t>(kNumChannels * kBytesPerSample));
  p += 2;
  write_u16(p, static_cast<uint16_t>(kBitsPerSample));
  p += 2;

  // data chunk.
  memcpy(p, "data", 4);
  p += 4;
  write_u32(p, data_size);
  p += 4;

  // Write samples (already float, just need to clamp to [-1, 1]).
  for (uint32_t i = 0; i < num_samples; ++i) {
    float s = std::max(-1.0f, std::min(1.0f, samples[i]));
    memcpy(p, &s, sizeof(float));
    p += sizeof(float);
  }

  return wav;
}

}  // namespace perfetto::trace_processor::sound_synth
