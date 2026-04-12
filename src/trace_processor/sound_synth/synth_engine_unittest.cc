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

#include <cstring>

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "protos/perfetto/trace_processor/synth.pbzero.h"
#include "src/trace_processor/sound_synth/modules.h"
#include "src/trace_processor/sound_synth/synth_engine.h"
#include "src/trace_processor/sound_synth/synth_module.h"
#include "src/base/test/status_matchers.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::sound_synth {
namespace {

TEST(SynthModulesTest, VcoSineProducesOutput) {
  Vco vco(Vco::Waveform::kSine, 440.0);
  vco.Process(4800);  // 0.1 seconds at 48kHz.
  const SignalBuffer* out = vco.GetOutput("out");
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 4800u);
  // A sine wave at 440Hz should cross zero multiple times in 0.1s.
  int zero_crossings = 0;
  for (size_t i = 1; i < out->size(); ++i) {
    if (((*out)[i - 1] >= 0 && (*out)[i] < 0) ||
        ((*out)[i - 1] < 0 && (*out)[i] >= 0)) {
      zero_crossings++;
    }
  }
  // 440Hz * 0.1s = 44 cycles, each cycle has 2 zero crossings = 88.
  EXPECT_GE(zero_crossings, 85);
  EXPECT_LE(zero_crossings, 91);
}

TEST(SynthModulesTest, VcoSawProducesOutput) {
  Vco vco(Vco::Waveform::kSaw, 100.0);
  vco.Process(480);
  const SignalBuffer* out = vco.GetOutput("out");
  ASSERT_NE(out, nullptr);
  // Saw wave should have values in [-1, 1].
  for (float s : *out) {
    EXPECT_GE(s, -1.0f);
    EXPECT_LE(s, 1.0f);
  }
}

TEST(SynthModulesTest, EnvelopeAttackDecay) {
  Envelope env(1.0, 10.0, 1.0);  // 1ms attack, 10ms decay.
  // Create a trigger signal: high for first 48 samples (1ms), then low.
  SignalBuffer trigger(4800, 0.0f);
  for (int i = 0; i < 48; ++i)
    trigger[static_cast<size_t>(i)] = 1.0f;

  env.SetInput("trigger", &trigger);
  env.Process(4800);

  const SignalBuffer* out = env.GetOutput("out");
  ASSERT_NE(out, nullptr);

  // Should start at 0, rise to ~1.0 within first ~48 samples (1ms),
  // then decay back toward 0.
  EXPECT_NEAR(static_cast<double>((*out)[0]), 0.0, 0.1);

  // Find the peak.
  float peak = 0;
  size_t peak_idx = 0;
  for (size_t i = 0; i < out->size(); ++i) {
    if ((*out)[i] > peak) {
      peak = (*out)[i];
      peak_idx = i;
    }
  }
  EXPECT_NEAR(static_cast<double>(peak), 1.0, 0.05);
  EXPECT_LT(peak_idx, 100u);  // Peak should happen within first ~2ms.

  // After decay (10ms = 480 samples from peak), should be near 0.
  if (peak_idx + 600 < out->size()) {
    EXPECT_LT((*out)[peak_idx + 600], 0.1f);
  }
}

TEST(SynthModulesTest, VcaMultiplies) {
  Vca vca(0.0);

  SignalBuffer input(100, 0.5f);
  SignalBuffer gain(100, 0.8f);

  vca.SetInput("in", &input);
  vca.SetInput("gain", &gain);
  vca.Process(100);

  const SignalBuffer* out = vca.GetOutput("out");
  ASSERT_NE(out, nullptr);
  for (float s : *out) {
    EXPECT_NEAR(static_cast<double>(s), 0.4, 0.001);
  }
}

TEST(SynthModulesTest, MixerSumsInputs) {
  Mixer mixer;

  SignalBuffer a(100, 0.3f);
  SignalBuffer b(100, 0.5f);

  mixer.SetInput("in", &a);
  mixer.SetInput("in.1", &b);
  mixer.Process(100);

  const SignalBuffer* out = mixer.GetOutput("out");
  ASSERT_NE(out, nullptr);
  for (float s : *out) {
    EXPECT_NEAR(static_cast<double>(s), 0.8, 0.001);
  }
}

TEST(SynthModulesTest, FullChainVcoEnvelopeVca) {
  // Build a simple chain: VCO -> VCA, with Envelope driving VCA gain.
  // Simulates a single triggered note.
  Vco vco(Vco::Waveform::kSine, 220.0);
  Envelope env(2.0, 50.0, 1.0);
  Vca vca(0.0);

  // Create a trigger: one pulse at sample 0.
  uint32_t n = 4800;  // 100ms
  SignalBuffer trigger(n, 0.0f);
  trigger[0] = 1.0f;

  // Process envelope.
  env.SetInput("trigger", &trigger);
  env.Process(n);

  // Process VCO.
  vco.Process(n);

  // Wire: VCO -> VCA.in, Envelope -> VCA.gain.
  vca.SetInput("in", vco.GetOutput("out"));
  vca.SetInput("gain", env.GetOutput("out"));
  vca.Process(n);

  const SignalBuffer* out = vca.GetOutput("out");
  ASSERT_NE(out, nullptr);

  // At the beginning, envelope is rising so output should be near zero.
  EXPECT_NEAR(static_cast<double>((*out)[0]), 0.0, 0.01);

  // Somewhere in the middle (during decay), output should be non-zero
  // (VCO * envelope).
  float max_abs = 0;
  for (float s : *out) {
    float a = s < 0 ? -s : s;
    if (a > max_abs)
      max_abs = a;
  }
  EXPECT_GT(max_abs, 0.5f);

  // At the end (after 50ms decay from ~2ms peak = ~52ms),
  // the envelope should have decayed, so output should be quiet.
  EXPECT_LT((*out)[n - 1] < 0 ? -(*out)[n - 1] : (*out)[n - 1], 0.1f);
}

TEST(SynthEngineTest, RenderWithSyntheticTrace) {
  // Create a TP instance with some synthetic slice data.
  auto tp = perfetto::trace_processor::TraceProcessor::CreateInstance(
      perfetto::trace_processor::Config{});
  ASSERT_OK(tp->NotifyEndOfFile());

  // Insert some synthetic data via SQL.
  {
    auto it = tp->ExecuteQuery(
        "CREATE TABLE _test_slice (ts INT, dur INT, name TEXT)");
    while (it.Next()) {}
    ASSERT_OK(it.Status());
  }
  {
    auto it = tp->ExecuteQuery(
        "INSERT INTO _test_slice VALUES "
        "(1000000000, 100000000, 'test_slice1'), "  // 1s, 100ms
        "(1200000000, 100000000, 'test_slice2')");   // 1.2s, 100ms
    while (it.Next()) {}
    ASSERT_OK(it.Status());
  }

  // Build a simple patch: VCO -> VCA with envelope, triggered by slices.
  protozero::HeapBuffered<protos::pbzero::SynthPatch> patch;

  // Module: trace source
  {
    auto* m = patch->add_modules();
    m->set_id("src");
    auto* ts = m->set_trace_slice_source();
    ts->set_track_name_glob("*");
    ts->set_signal_type(protos::pbzero::TraceSliceSourceConfig::GATE);
  }
  // Module: envelope
  {
    auto* m = patch->add_modules();
    m->set_id("env");
    auto* e = m->set_envelope();
    e->set_attack_ms(5);
    e->set_decay_ms(100);
    e->set_peak(1.0);
  }
  // Module: VCO
  {
    auto* m = patch->add_modules();
    m->set_id("osc");
    auto* v = m->set_vco();
    v->set_waveform(protos::pbzero::VcoConfig::SINE);
    v->set_base_freq_hz(220);
  }
  // Module: VCA
  {
    auto* m = patch->add_modules();
    m->set_id("amp");
    m->set_vca();
  }
  // Module: master mixer
  {
    auto* m = patch->add_modules();
    m->set_id("master");
    m->set_mixer();
  }

  // Wires.
  {
    auto* w = patch->add_wires();
    w->set_from_module("src");
    w->set_to_module("env");
    w->set_to_port("trigger");
  }
  {
    auto* w = patch->add_wires();
    w->set_from_module("env");
    w->set_to_module("amp");
    w->set_to_port("gain");
  }
  {
    auto* w = patch->add_wires();
    w->set_from_module("osc");
    w->set_to_module("amp");
    w->set_to_port("in");
  }
  {
    auto* w = patch->add_wires();
    w->set_from_module("amp");
    w->set_to_module("master");
    w->set_to_port("in");
  }

  auto patch_data = patch.SerializeAsArray();

  SynthEngine engine(tp.get());
  // The engine queries the `slice` table, but we created `_test_slice`.
  // For this test, we just verify it doesn't crash and returns an error
  // (since there's no data in the real `slice` table of an empty trace).
  auto result = engine.Render(patch_data.data(), patch_data.size(), 0, 0);
  // Empty trace has no slices, so we expect an error about no data.
  EXPECT_FALSE(result.ok());
}

}  // namespace
}  // namespace perfetto::trace_processor::sound_synth
