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

#include "src/trace_processor/shell/techno_subcommand.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/trace_processor/synth.pbzero.h"
#include "src/trace_processor/shell/common_flags.h"
#include "src/trace_processor/shell/shell_utils.h"
#include "src/trace_processor/shell/subcommand.h"
#include "src/trace_processor/sound_synth/synth_engine.h"

namespace perfetto::trace_processor::shell {

const char* TechnoSubcommand::name() const {
  return "techno";
}

const char* TechnoSubcommand::description() const {
  return "Synthesize a techno WAV from a trace.";
}

const char* TechnoSubcommand::usage_args() const {
  return "-o FILE [--patch-file FILE | <trace_file>]";
}

const char* TechnoSubcommand::detailed_help() const {
  return R"(Synthesize a WAV file from a synth patch.

Two rendering modes:

 1. Trace-driven (default): load a trace file and run the built-in demo
    patch (trace slices -> envelope -> sine bass).

      trace_processor_shell techno -o out.wav trace.perfetto-trace

 2. Patch-driven: load a binary SynthPatch proto file and render it for a
    fixed duration without needing a trace. Used by the preset rendering
    pipeline (tools/trace_to_techno/render_all_presets.py).

      trace_processor_shell techno -o out.wav --patch-file preset.pb \
          --duration-secs 16

Flags:
  -o FILE             Output WAV file path (required).
  --patch-file FILE   Binary SynthPatch proto (disables trace loading).
  --duration-secs N   Render duration in seconds (patch-file mode).
  --pcm24             Output 24-bit PCM instead of 32-bit float.

The default output is a 48 kHz 32-bit float mono WAV.)";
}

std::vector<FlagSpec> TechnoSubcommand::GetFlags() {
  return {
      StringFlag("output", 'o', "FILE", "Output WAV file path.", &output_path_),
      StringFlag("patch-file", '\0', "FILE",
                 "Binary SynthPatch proto (skips trace loading).",
                 &patch_path_),
      StringFlag("duration-secs", '\0', "N",
                 "Render duration in seconds (patch-file mode).",
                 &duration_secs_str_),
      BoolFlag("pcm24", '\0', "Output 24-bit PCM instead of 32-bit float.",
               &pcm24_),
  };
}

namespace {

// Builds a default synth patch: trace slices -> envelope -> VCA <- VCO.
std::vector<uint8_t> BuildDefaultPatch() {
  protozero::HeapBuffered<protos::pbzero::SynthPatch> patch;

  // Trace source (gate from all slices).
  {
    auto* m = patch->add_modules();
    m->set_id("src");
    auto* ts = m->set_trace_slice_source();
    ts->set_track_name_glob("*");
    ts->set_signal_type(protos::pbzero::TraceSliceSourceConfig::GATE);
  }
  // Envelope.
  {
    auto* m = patch->add_modules();
    m->set_id("env");
    auto* e = m->set_envelope();
    e->set_attack_ms(2);
    e->set_decay_ms(80);
    e->set_peak(1.0);
  }
  // VCO (sine at 110 Hz - bass).
  {
    auto* m = patch->add_modules();
    m->set_id("osc");
    auto* v = m->set_vco();
    v->set_waveform(protos::pbzero::VcoConfig::SINE);
    v->set_base_freq_hz(110);
  }
  // VCA.
  {
    auto* m = patch->add_modules();
    m->set_id("amp");
    m->set_vca();
  }
  // Master mixer.
  {
    auto* m = patch->add_modules();
    m->set_id("master");
    m->set_mixer();
  }

  // Wires.
  auto add_wire = [&](const char* from, const char* to, const char* to_port) {
    auto* w = patch->add_wires();
    w->set_from_module(from);
    w->set_to_module(to);
    w->set_to_port(to_port);
  };
  add_wire("src", "env", "trigger");
  add_wire("env", "amp", "gain");
  add_wire("osc", "amp", "in");
  add_wire("amp", "master", "in");

  return patch.SerializeAsArray();
}

// Converts a 32-bit float WAV to 24-bit PCM WAV in place.
std::vector<uint8_t> ConvertToPcm24(const std::vector<uint8_t>& float_wav) {
  // Parse the float WAV header to get sample count.
  // Header is 44 bytes: RIFF(12) + fmt(24) + data(8) + samples.
  uint32_t data_size = 0;
  memcpy(&data_size, float_wav.data() + 40, 4);
  uint32_t num_samples = data_size / 4;  // 4 bytes per float sample.

  uint32_t pcm_data_size = num_samples * 3;  // 3 bytes per 24-bit sample.
  uint32_t pcm_file_size = 44 + pcm_data_size;

  std::vector<uint8_t> pcm(pcm_file_size);
  uint8_t* p = pcm.data();

  auto write_u32 = [](uint8_t* dst, uint32_t val) { memcpy(dst, &val, 4); };
  auto write_u16 = [](uint8_t* dst, uint16_t val) { memcpy(dst, &val, 2); };

  // RIFF header.
  memcpy(p, "RIFF", 4);
  p += 4;
  write_u32(p, pcm_file_size - 8);
  p += 4;
  memcpy(p, "WAVE", 4);
  p += 4;

  // fmt chunk.
  memcpy(p, "fmt ", 4);
  p += 4;
  write_u32(p, 16);
  p += 4;
  write_u16(p, 1);  // Audio format: PCM.
  p += 2;
  write_u16(p, 1);  // Channels: mono.
  p += 2;
  write_u32(p, 48000);  // Sample rate.
  p += 4;
  write_u32(p, 48000 * 3);  // Byte rate: 48000 * 1ch * 3 bytes.
  p += 4;
  write_u16(p, 3);  // Block align: 1ch * 3 bytes.
  p += 2;
  write_u16(p, 24);  // Bits per sample.
  p += 2;

  // data chunk.
  memcpy(p, "data", 4);
  p += 4;
  write_u32(p, pcm_data_size);
  p += 4;

  // Convert float samples to 24-bit signed integer (little-endian).
  const uint8_t* src = float_wav.data() + 44;
  for (uint32_t i = 0; i < num_samples; ++i) {
    float s = 0;
    memcpy(&s, src + i * 4, sizeof(float));
    s = std::max(-1.0f, std::min(1.0f, s));
    // Scale to 24-bit range: [-8388608, 8388607].
    auto val =
        static_cast<int32_t>(std::round(static_cast<double>(s) * 8388607.0));
    // Write 3 bytes little-endian.
    p[0] = static_cast<uint8_t>(val & 0xFF);
    p[1] = static_cast<uint8_t>((val >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((val >> 16) & 0xFF);
    p += 3;
  }

  return pcm;
}

}  // namespace

base::Status TechnoSubcommand::Run(const SubcommandContext& ctx) {
  if (output_path_.empty()) {
    return base::ErrStatus("techno: -o FILE is required");
  }

  // Load either a user-supplied patch file or fall back to the built-in demo.
  std::vector<uint8_t> patch_data;
  if (!patch_path_.empty()) {
    std::ifstream f(patch_path_, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
      return base::ErrStatus("techno: cannot open patch file '%s'",
                             patch_path_.c_str());
    }
    auto size = f.tellg();
    if (size < 0) {
      return base::ErrStatus("techno: cannot read patch file '%s'",
                             patch_path_.c_str());
    }
    patch_data.resize(static_cast<size_t>(size));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(patch_data.data()),
           static_cast<std::streamsize>(size));
    if (!f) {
      return base::ErrStatus("techno: short read on patch file '%s'",
                             patch_path_.c_str());
    }
  } else {
    patch_data = BuildDefaultPatch();
  }

  // Parse --duration-secs if given.
  double duration_secs = 0.0;
  if (!duration_secs_str_.empty()) {
    char* end = nullptr;
    duration_secs = std::strtod(duration_secs_str_.c_str(), &end);
    if (end == duration_secs_str_.c_str() || duration_secs <= 0.0) {
      return base::ErrStatus("techno: invalid --duration-secs '%s'",
                             duration_secs_str_.c_str());
    }
  }

  // Decide whether to load a trace. If --duration-secs is given we can
  // render a patch without a trace. Otherwise a trace is required to
  // drive any TraceSliceSource modules.
  auto config = BuildConfig(*ctx.global, ctx.platform);
  ASSIGN_OR_RETURN(auto tp,
                   SetupTraceProcessor(*ctx.global, config, ctx.platform));

  bool have_trace = false;
  if (!ctx.positional_args.empty()) {
    const std::string& trace_file = ctx.positional_args[0];
    RETURN_IF_ERROR(LoadTraceFile(tp.get(), ctx.platform, trace_file).status());
    have_trace = true;
  } else if (duration_secs <= 0.0) {
    return base::ErrStatus(
        "techno: either a <trace_file> positional or --duration-secs N is "
        "required");
  }
  (void)have_trace;

  sound_synth::SynthEngine engine(tp.get());
  ASSIGN_OR_RETURN(auto wav, engine.Render(patch_data.data(), patch_data.size(),
                                           0, 0, duration_secs));

  const std::vector<uint8_t>* output = &wav;
  std::vector<uint8_t> pcm_wav;
  if (pcm24_) {
    pcm_wav = ConvertToPcm24(wav);
    output = &pcm_wav;
  }

  std::ofstream out(output_path_, std::ios::binary);
  if (!out.is_open()) {
    return base::ErrStatus("techno: cannot open output file '%s'",
                           output_path_.c_str());
  }
  out.write(reinterpret_cast<const char*>(output->data()),
            static_cast<std::streamsize>(output->size()));

  uint32_t bytes_per_sample = pcm24_ ? 3 : 4;
  fprintf(stderr, "Wrote %zu bytes to %s (%.2f seconds, %s)\n", output->size(),
          output_path_.c_str(),
          static_cast<double>(output->size() - 44) /
              (48000.0 * static_cast<double>(bytes_per_sample)),
          pcm24_ ? "48kHz 24-bit PCM mono" : "48kHz 32-bit float mono");
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::shell
