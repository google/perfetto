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
#include "src/trace_processor/sound_synth/effects.h"
#include "src/trace_processor/sound_synth/filters.h"
#include "src/trace_processor/sound_synth/modulators.h"
#include "src/trace_processor/sound_synth/oscillators.h"
#include "src/trace_processor/sound_synth/sources.h"
#include "src/trace_processor/sound_synth/synth_module.h"
#include "src/trace_processor/sound_synth/utility.h"

#include "protos/perfetto/trace_processor/synth.pbzero.h"

namespace perfetto::trace_processor::sound_synth {

namespace {

using SynthPatchProto = protos::pbzero::SynthPatch;
using SynthModuleProto = protos::pbzero::SynthModule;
using WireProto = protos::pbzero::SynthWire;
using SliceSourceProto = protos::pbzero::TraceSliceSourceConfig;
using TestPatternProto = protos::pbzero::TestPatternSourceConfig;
using VcoProto = protos::pbzero::VcoConfig;
using VcaProto = protos::pbzero::VcaConfig;
using EnvProto = protos::pbzero::EnvelopeConfig;
using AdsrProto = protos::pbzero::AdsrConfig;
using ClassicOscProto = protos::pbzero::ClassicOscConfig;
using NoiseOscProto = protos::pbzero::NoiseOscConfig;
using WavetableOscProto = protos::pbzero::WavetableOscConfig;
using FmOscProto = protos::pbzero::FmOscConfig;
using PhaseDistortionOscProto = protos::pbzero::PhaseDistortionOscConfig;
using FoldOscProto = protos::pbzero::FoldOscConfig;
using SyncOscProto = protos::pbzero::SyncOscConfig;
using SuperOscProto = protos::pbzero::SuperOscConfig;
using LfoProto = protos::pbzero::LfoConfig;
using WaveshaperProto = protos::pbzero::WaveshaperConfig;
using MoogLadderProto = protos::pbzero::MoogLadderConfig;
using SvfProto = protos::pbzero::SvfConfig;
using DelayProto = protos::pbzero::DelayConfig;
using ChorusProto = protos::pbzero::ChorusConfig;
using DrawbarOrganProto = protos::pbzero::DrawbarOrganConfig;

}  // namespace

SynthEngine::SynthEngine(TraceProcessor* tp) : tp_(tp) {}
SynthEngine::~SynthEngine() = default;

void SynthEngine::ApplyPostProcessTransforms(SynthModule* mod,
                                             uint32_t num_samples) {
  for (const auto& op : transform_ops_) {
    if (op.source != mod)
      continue;
    // Resize destination to match the source buffer (handles the first
    // fill as well as any later resizes if the module chose a larger size).
    op.dst_buf->resize(num_samples);
    const uint32_t n =
        std::min(num_samples, static_cast<uint32_t>(op.src_buf->size()));
    for (uint32_t i = 0; i < n; ++i) {
      (*op.dst_buf)[i] = static_cast<float>(
          static_cast<double>((*op.src_buf)[i]) * op.scale + op.offset);
    }
    // Zero any tail if src was shorter.
    for (uint32_t i = n; i < num_samples; ++i) {
      (*op.dst_buf)[i] = static_cast<float>(op.offset);
    }
  }
}

base::StatusOr<std::vector<uint8_t>> SynthEngine::Render(
    const uint8_t* patch_data,
    size_t patch_size,
    int64_t start_ts,
    int64_t end_ts,
    double duration_seconds) {
  modules_.clear();
  wires_.clear();
  processing_order_.clear();
  transform_buffers_.clear();
  transform_ops_.clear();

  // Cap the final rendered audio to avoid OOM on pathological requests.
  // ~2 minutes of audio at 48 kHz mono float ≈ 23 MB.
  constexpr double kMaxAudioSecs = 120.0;

  double duration_secs = 0.0;
  bool skip_trace_source_population = false;

  if (duration_seconds > 0.0) {
    // Preset/preview mode: explicit length, no trace query.
    duration_secs = duration_seconds;
    skip_trace_source_population = true;
  } else {
    // Legacy path: compute duration from trace timestamps.
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

    double trace_duration_secs =
        static_cast<double>(end_ts - start_ts) / 1000000000.0;
    // Safety cap before time dilation: 10 s trace * 48x dilation = 8 min
    // audio ≈ 90 MB.
    constexpr double kMaxTraceDurationSecs = 10.0;
    if (trace_duration_secs > kMaxTraceDurationSecs) {
      return base::ErrStatus(
          "Requested trace window (%.1f s) exceeds max (%.1f s). "
          "Zoom in or pass a smaller [start_ts, end_ts] range.",
          trace_duration_secs, kMaxTraceDurationSecs);
    }
    duration_secs = trace_duration_secs * kTimeDilation;
  }

  if (duration_secs <= 0)
    return base::ErrStatus("Invalid render duration");
  if (duration_secs > kMaxAudioSecs) {
    return base::ErrStatus(
        "Requested audio duration (%.1f s) exceeds max (%.1f s).",
        duration_secs, kMaxAudioSecs);
  }
  auto num_samples = static_cast<uint32_t>(duration_secs * kSampleRate);
  if (num_samples == 0)
    return base::ErrStatus("Render duration too short");

  // 1. Parse patch config and create modules.
  RETURN_IF_ERROR(BuildModules(patch_data, patch_size));

  // 2. Fill trace source buffers from trace data (unless we're in
  // preset-preview mode, in which case we have no trace window anyway
  // and any TraceSliceSource just gets an all-zero buffer).
  if (!skip_trace_source_population) {
    RETURN_IF_ERROR(PopulateTraceSources(start_ts, end_ts, num_samples));
  }

  // 3. Determine processing order (topological sort).
  TopoSort();

  // 4. Connect wires and process.
  RETURN_IF_ERROR(ConnectWires(num_samples));

  // 5. Process all modules in order. After each module we apply any
  // deferred wire transforms that read from its output, so downstream
  // modules see the correctly-transformed buffer when they run.
  for (auto* mod : processing_order_) {
    mod->Process(num_samples);
    ApplyPostProcessTransforms(mod, num_samples);
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
    } else if (mod.has_test_pattern_source()) {
      TestPatternProto::Decoder cfg(mod.test_pattern_source());
      TestPatternSource::Mode mode = TestPatternSource::Mode::kArpeggio;
      if (cfg.has_mode() && cfg.mode() == TestPatternProto::IMPULSES) {
        mode = TestPatternSource::Mode::kImpulses;
      }
      uint32_t num_hits = cfg.has_num_hits() ? cfg.num_hits() : 16;
      double bpm = cfg.has_bpm() ? cfg.bpm() : 128.0;
      uint32_t bars = cfg.has_bars() ? cfg.bars() : 8;
      m = std::make_unique<TestPatternSource>(mode, num_hits, bpm, bars);
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
    } else if (mod.has_adsr()) {
      AdsrProto::Decoder cfg(mod.adsr());
      double attack = cfg.has_attack_ms() ? cfg.attack_ms() : 5.0;
      double decay = cfg.has_decay_ms() ? cfg.decay_ms() : 100.0;
      double sustain = cfg.has_sustain() ? cfg.sustain() : 0.7;
      double release = cfg.has_release_ms() ? cfg.release_ms() : 200.0;
      m = std::make_unique<Adsr>(attack, decay, sustain, release);
    } else if (mod.has_classic_osc()) {
      ClassicOscProto::Decoder cfg(mod.classic_osc());
      ClassicOsc::Waveform wf = ClassicOsc::Waveform::kSaw;
      if (cfg.has_waveform()) {
        switch (cfg.waveform()) {
          case ClassicOscProto::SQUARE:
            wf = ClassicOsc::Waveform::kSquare;
            break;
          case ClassicOscProto::TRIANGLE:
            wf = ClassicOsc::Waveform::kTriangle;
            break;
          case ClassicOscProto::SINE:
            wf = ClassicOsc::Waveform::kSine;
            break;
          case ClassicOscProto::SAW:
          default:
            wf = ClassicOsc::Waveform::kSaw;
            break;
        }
      }
      double freq = cfg.has_base_freq_hz() ? cfg.base_freq_hz() : 440.0;
      double pw = cfg.has_pulse_width() ? cfg.pulse_width() : 0.5;
      m = std::make_unique<ClassicOsc>(wf, freq, pw);
    } else if (mod.has_noise_osc()) {
      NoiseOscProto::Decoder cfg(mod.noise_osc());
      double tilt = cfg.has_tilt() ? cfg.tilt() : 0.0;
      uint32_t seed = cfg.has_seed() ? cfg.seed() : 0u;
      m = std::make_unique<NoiseOsc>(tilt, seed);
    } else if (mod.has_wavetable_osc()) {
      WavetableOscProto::Decoder cfg(mod.wavetable_osc());
      WavetableOsc::TableType tt = WavetableOsc::TableType::kSineToSaw;
      if (cfg.has_table_type()) {
        switch (cfg.table_type()) {
          case WavetableOscProto::PULSE_SWEEP:
            tt = WavetableOsc::TableType::kPulseSweep;
            break;
          case WavetableOscProto::BELL:
            tt = WavetableOsc::TableType::kBell;
            break;
          case WavetableOscProto::VOCAL:
            tt = WavetableOsc::TableType::kVocal;
            break;
          case WavetableOscProto::SINE_TO_SAW:
          default:
            tt = WavetableOsc::TableType::kSineToSaw;
            break;
        }
      }
      double freq = cfg.has_base_freq_hz() ? cfg.base_freq_hz() : 220.0;
      double pos = cfg.has_base_position() ? cfg.base_position() : 0.0;
      m = std::make_unique<WavetableOsc>(tt, freq, pos);
    } else if (mod.has_fm_osc()) {
      FmOscProto::Decoder cfg(mod.fm_osc());
      double f = cfg.has_base_freq_hz() ? cfg.base_freq_hz() : 220.0;
      double ratio = cfg.has_mod_ratio() ? cfg.mod_ratio() : 1.0;
      double index = cfg.has_mod_index() ? cfg.mod_index() : 1.0;
      double fb = cfg.has_feedback() ? cfg.feedback() : 0.0;
      m = std::make_unique<FmOsc>(f, ratio, index, fb);
    } else if (mod.has_phase_distortion_osc()) {
      PhaseDistortionOscProto::Decoder cfg(mod.phase_distortion_osc());
      PhaseDistortionOsc::Mode pdm = PhaseDistortionOsc::Mode::kSawWarp;
      if (cfg.has_mode() && cfg.mode() == PhaseDistortionOscProto::PULSE_WARP) {
        pdm = PhaseDistortionOsc::Mode::kPulseWarp;
      }
      double f = cfg.has_base_freq_hz() ? cfg.base_freq_hz() : 220.0;
      double amt = cfg.has_amount() ? cfg.amount() : 0.5;
      m = std::make_unique<PhaseDistortionOsc>(pdm, f, amt);
    } else if (mod.has_fold_osc()) {
      FoldOscProto::Decoder cfg(mod.fold_osc());
      double f = cfg.has_base_freq_hz() ? cfg.base_freq_hz() : 220.0;
      double drive = cfg.has_drive() ? cfg.drive() : 3.0;
      double bias = cfg.has_bias() ? cfg.bias() : 0.0;
      m = std::make_unique<FoldOsc>(f, drive, bias);
    } else if (mod.has_sync_osc()) {
      SyncOscProto::Decoder cfg(mod.sync_osc());
      double f = cfg.has_base_freq_hz() ? cfg.base_freq_hz() : 110.0;
      double ratio = cfg.has_sync_ratio() ? cfg.sync_ratio() : 2.0;
      m = std::make_unique<SyncOsc>(f, ratio);
    } else if (mod.has_super_osc()) {
      SuperOscProto::Decoder cfg(mod.super_osc());
      double f = cfg.has_base_freq_hz() ? cfg.base_freq_hz() : 220.0;
      double detune = cfg.has_detune() ? cfg.detune() : 0.3;
      double mix = cfg.has_mix() ? cfg.mix() : 0.5;
      m = std::make_unique<SuperOsc>(f, detune, mix);
    } else if (mod.has_drawbar_organ()) {
      DrawbarOrganProto::Decoder cfg(mod.drawbar_organ());
      double f = cfg.has_base_freq_hz() ? cfg.base_freq_hz() : 440.0;
      double levels[9] = {
          cfg.has_db16() ? cfg.db16() : 0.0,
          cfg.has_db5_1_3() ? cfg.db5_1_3() : 0.0,
          cfg.has_db8() ? cfg.db8() : 1.0,
          cfg.has_db4() ? cfg.db4() : 0.0,
          cfg.has_db2_2_3() ? cfg.db2_2_3() : 0.0,
          cfg.has_db2() ? cfg.db2() : 0.0,
          cfg.has_db1_3_5() ? cfg.db1_3_5() : 0.0,
          cfg.has_db1_1_3() ? cfg.db1_1_3() : 0.0,
          cfg.has_db1() ? cfg.db1() : 0.0,
      };
      m = std::make_unique<DrawbarOrgan>(f, levels);
    } else if (mod.has_chorus()) {
      ChorusProto::Decoder cfg(mod.chorus());
      double rate = cfg.has_rate_hz() ? cfg.rate_hz() : 0.5;
      double depth = cfg.has_depth_ms() ? cfg.depth_ms() : 4.0;
      double mid = cfg.has_mid_delay_ms() ? cfg.mid_delay_ms() : 15.0;
      double mix = cfg.has_mix() ? cfg.mix() : 0.5;
      uint32_t voices = cfg.has_voices() ? cfg.voices() : 3u;
      m = std::make_unique<Chorus>(rate, depth, mid, mix, voices);
    } else if (mod.has_lfo()) {
      LfoProto::Decoder cfg(mod.lfo());
      Lfo::Waveform wf = Lfo::Waveform::kSine;
      if (cfg.has_waveform()) {
        switch (cfg.waveform()) {
          case LfoProto::TRIANGLE:
            wf = Lfo::Waveform::kTriangle;
            break;
          case LfoProto::SQUARE:
            wf = Lfo::Waveform::kSquare;
            break;
          case LfoProto::SAW_UP:
            wf = Lfo::Waveform::kSawUp;
            break;
          case LfoProto::SAW_DOWN:
            wf = Lfo::Waveform::kSawDown;
            break;
          case LfoProto::SAMPLE_AND_HOLD:
            wf = Lfo::Waveform::kSampleAndHold;
            break;
          case LfoProto::SINE:
          default:
            wf = Lfo::Waveform::kSine;
            break;
        }
      }
      double rate = cfg.has_rate_hz() ? cfg.rate_hz() : 1.0;
      double depth = cfg.has_depth() ? cfg.depth() : 1.0;
      bool bipolar = cfg.has_bipolar() ? cfg.bipolar() : true;
      uint32_t seed = cfg.has_seed() ? cfg.seed() : 0u;
      m = std::make_unique<Lfo>(wf, rate, depth, bipolar, seed);
    } else if (mod.has_waveshaper()) {
      WaveshaperProto::Decoder cfg(mod.waveshaper());
      Waveshaper::Mode ws_mode = Waveshaper::Mode::kSoftTanh;
      if (cfg.has_mode()) {
        switch (cfg.mode()) {
          case WaveshaperProto::HARD_CLIP:
            ws_mode = Waveshaper::Mode::kHardClip;
            break;
          case WaveshaperProto::FOLD:
            ws_mode = Waveshaper::Mode::kFold;
            break;
          case WaveshaperProto::ASYMMETRIC:
            ws_mode = Waveshaper::Mode::kAsymmetric;
            break;
          case WaveshaperProto::SOFT_TANH:
          default:
            ws_mode = Waveshaper::Mode::kSoftTanh;
            break;
        }
      }
      double drive = cfg.has_drive() ? cfg.drive() : 2.0;
      double mix = cfg.has_mix() ? cfg.mix() : 1.0;
      m = std::make_unique<Waveshaper>(ws_mode, drive, mix);
    } else if (mod.has_moog_ladder()) {
      MoogLadderProto::Decoder cfg(mod.moog_ladder());
      double cutoff = cfg.has_base_cutoff_hz() ? cfg.base_cutoff_hz() : 1000.0;
      double reso = cfg.has_base_resonance() ? cfg.base_resonance() : 0.0;
      double drive = cfg.has_drive() ? cfg.drive() : 1.0;
      m = std::make_unique<MoogLadder>(cutoff, reso, drive);
    } else if (mod.has_svf()) {
      SvfProto::Decoder cfg(mod.svf());
      Svf::Mode svf_mode = Svf::Mode::kLowpass;
      if (cfg.has_mode()) {
        switch (cfg.mode()) {
          case SvfProto::HIGHPASS:
            svf_mode = Svf::Mode::kHighpass;
            break;
          case SvfProto::BANDPASS:
            svf_mode = Svf::Mode::kBandpass;
            break;
          case SvfProto::NOTCH:
            svf_mode = Svf::Mode::kNotch;
            break;
          case SvfProto::LOWPASS:
          default:
            svf_mode = Svf::Mode::kLowpass;
            break;
        }
      }
      double cutoff = cfg.has_base_cutoff_hz() ? cfg.base_cutoff_hz() : 1000.0;
      double q = cfg.has_base_q() ? cfg.base_q() : 1.0;
      m = std::make_unique<Svf>(svf_mode, cutoff, q);
    } else if (mod.has_delay()) {
      DelayProto::Decoder cfg(mod.delay());
      double time = cfg.has_time_ms() ? cfg.time_ms() : 250.0;
      double fb = cfg.has_feedback() ? cfg.feedback() : 0.4;
      double damp = cfg.has_damping() ? cfg.damping() : 0.3;
      double mix = cfg.has_mix() ? cfg.mix() : 0.4;
      m = std::make_unique<Delay>(time, fb, damp, mix);
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
  double ns_per_sample = static_cast<double>(end_ts - start_ts) / num_samples;

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
      auto s_start = static_cast<int64_t>(static_cast<double>(ts - start_ts) /
                                          ns_per_sample);
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
                             wire.from_module.c_str(), wire.from_port.c_str());

    // If scale/offset are non-trivial, allocate an intermediate buffer now
    // and defer the actual transform until after the source module has
    // produced its output. Attempting to transform eagerly here fails
    // because src_buf is empty at wire-connect time (modules haven't run
    // yet).
    const SignalBuffer* connect_buf = src_buf;
    if (wire.scale != 1.0 || wire.offset != 0.0) {
      auto transformed = std::make_unique<SignalBuffer>(num_samples, 0.0f);
      TransformOp op;
      op.source = from;
      op.src_buf = src_buf;
      op.dst_buf = transformed.get();
      op.scale = wire.scale;
      op.offset = wire.offset;
      transform_ops_.push_back(op);
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
  std::function<void(const std::string&)> visit = [&](const std::string& id) {
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

  auto write_u32 = [](uint8_t* dst, uint32_t val) { memcpy(dst, &val, 4); };
  auto write_u16 = [](uint8_t* dst, uint16_t val) { memcpy(dst, &val, 2); };

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
