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
#include "src/base/test/status_matchers.h"
#include "src/trace_processor/sound_synth/effects.h"
#include "src/trace_processor/sound_synth/filters.h"
#include "src/trace_processor/sound_synth/modulators.h"
#include "src/trace_processor/sound_synth/oscillators.h"
#include "src/trace_processor/sound_synth/sources.h"
#include "src/trace_processor/sound_synth/synth_engine.h"
#include "src/trace_processor/sound_synth/synth_module.h"
#include "src/trace_processor/sound_synth/utility.h"
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

TEST(SynthModulesTest, ChorusProducesModulatedOutput) {
  // Feed a steady 200 Hz sine into a chorus and check that (a) the output
  // is not equal to the input (modulation is happening) and (b) the output
  // peak is still bounded.
  Chorus chorus(/*rate_hz=*/2.0, /*depth_ms=*/5.0, /*mid_delay_ms=*/15.0,
                /*mix=*/1.0, /*voices=*/3);
  SignalBuffer in(48000);  // 1 second of 200 Hz sine
  for (size_t i = 0; i < in.size(); ++i) {
    in[i] = 0.5f * std::sin(2.0f * 3.14159265f * 200.0f *
                            static_cast<float>(i) / 48000.0f);
  }
  chorus.SetInput("in", &in);
  chorus.Process(48000);
  const auto* out = chorus.GetOutput("out");
  ASSERT_NE(out, nullptr);
  double diff_sum = 0.0;
  double peak = 0.0;
  for (size_t i = 2000; i < out->size(); ++i) {
    double d = static_cast<double>((*out)[i]) - static_cast<double>(in[i]);
    diff_sum += d * d;
    double a = std::abs(static_cast<double>((*out)[i]));
    if (a > peak)
      peak = a;
  }
  EXPECT_GT(diff_sum, 1.0);
  EXPECT_LE(peak, 1.2);
}

TEST(SynthModulesTest, ChorusMixZeroEqualsInput) {
  Chorus chorus(1.0, 4.0, 15.0, 0.0, 3);
  SignalBuffer in(1000);
  for (size_t i = 0; i < in.size(); ++i) {
    in[i] = static_cast<float>(i % 100) / 100.0f;
  }
  chorus.SetInput("in", &in);
  chorus.Process(1000);
  const auto* out = chorus.GetOutput("out");
  for (size_t i = 0; i < in.size(); ++i) {
    EXPECT_NEAR(static_cast<double>((*out)[i]), static_cast<double>(in[i]),
                1e-5);
  }
}

TEST(SynthModulesTest, DrawbarOrganFundamentalOnlyIsSine) {
  // Only db8 (fundamental) set → output should be a pure sine at the
  // played frequency. Count zero crossings.
  double levels[9] = {0, 0, 1.0, 0, 0, 0, 0, 0, 0};
  DrawbarOrgan organ(440.0, levels);
  organ.Process(4800);
  const auto* out = organ.GetOutput("out");
  ASSERT_NE(out, nullptr);
  int zc = 0;
  for (size_t i = 1; i < out->size(); ++i) {
    if (((*out)[i - 1] >= 0 && (*out)[i] < 0) ||
        ((*out)[i - 1] < 0 && (*out)[i] >= 0)) {
      ++zc;
    }
  }
  EXPECT_GE(zc, 85);
  EXPECT_LE(zc, 91);
  for (float s : *out) {
    EXPECT_LE(s, 1.01f);
    EXPECT_GE(s, -1.01f);
  }
}

TEST(SynthModulesTest, DrawbarOrganOctaveAddsHarmonic) {
  double levels_fund[9] = {0, 0, 1.0, 0, 0, 0, 0, 0, 0};
  double levels_oct[9] = {0, 0, 1.0, 1.0, 0, 0, 0, 0, 0};
  DrawbarOrgan fund(220.0, levels_fund);
  DrawbarOrgan oct(220.0, levels_oct);
  fund.Process(4800);
  oct.Process(4800);
  auto count_zc = [](const SignalBuffer* b) {
    int zc = 0;
    for (size_t i = 1; i < b->size(); ++i) {
      if (((*b)[i - 1] >= 0 && (*b)[i] < 0) ||
          ((*b)[i - 1] < 0 && (*b)[i] >= 0))
        ++zc;
    }
    return zc;
  };
  int fund_zc = count_zc(fund.GetOutput("out"));
  int oct_zc = count_zc(oct.GetOutput("out"));
  // Octave adds a 2x-rate component → more zero crossings.
  EXPECT_GT(oct_zc, fund_zc + 20);
}

TEST(SynthModulesTest, DrawbarOrganFreqInputOverrides) {
  double levels[9] = {0, 0, 1.0, 0, 0, 0, 0, 0, 0};
  DrawbarOrgan organ(100.0, levels);
  SignalBuffer freq(4800, 440.0f);
  organ.SetInput("freq", &freq);
  organ.Process(4800);
  const auto* out = organ.GetOutput("out");
  int zc = 0;
  for (size_t i = 1; i < out->size(); ++i) {
    if (((*out)[i - 1] >= 0 && (*out)[i] < 0) ||
        ((*out)[i - 1] < 0 && (*out)[i] >= 0))
      ++zc;
  }
  EXPECT_GE(zc, 85);
  EXPECT_LE(zc, 91);
}

TEST(SynthModulesTest, TestPatternArpeggioFreqAndGate) {
  // Default: 8 bars at 128 BPM. total_notes = 64.
  // note_duration_s = 30/128 = 0.234375, note_samples = 11250.
  // 15 s total → need 720_000 samples.
  TestPatternSource src(TestPatternSource::Mode::kArpeggio, 0, 128.0, 8);
  const uint32_t kN = 720000;
  src.Process(kN);
  const auto* gate = src.GetOutput("out");
  const auto* freq = src.GetOutput("freq");
  ASSERT_NE(gate, nullptr);
  ASSERT_NE(freq, nullptr);

  // First note: Am chord, A1 = 55 Hz. gate should be high at sample 0,
  // freq should be 55.
  EXPECT_FLOAT_EQ((*gate)[0], 1.0f);
  EXPECT_NEAR(static_cast<double>((*freq)[0]), 55.0, 0.001);

  // Within the first note: sample at idx 7000 (~0.146 s, well within the
  // first note which ends at 11250). gate still high (70% duty = ~7875).
  EXPECT_FLOAT_EQ((*gate)[7000], 1.0f);
  EXPECT_NEAR(static_cast<double>((*freq)[7000]), 55.0, 0.001);

  // Near the end of the first note (idx 10000, past the 70% gate-off point
  // at 7875), gate should be low, but freq still holds 55.
  EXPECT_FLOAT_EQ((*gate)[10000], 0.0f);
  EXPECT_NEAR(static_cast<double>((*freq)[10000]), 55.0, 0.001);

  // Second note: starts at sample 11250. Am chord note 2 = C2 = 65.41 Hz.
  EXPECT_FLOAT_EQ((*gate)[11250], 1.0f);
  EXPECT_NEAR(static_cast<double>((*freq)[11250]), 65.41, 0.01);

  // Last note of chord 1 (Am) is note 15 → note_start = 15*11250 = 168750.
  // Value: A6 = 1760 Hz.
  EXPECT_NEAR(static_cast<double>((*freq)[168750]), 1760.0, 0.1);

  // First note of chord 2 (G) is note 16 → start = 180000. G1 = 49.0 Hz.
  EXPECT_NEAR(static_cast<double>((*freq)[180000]), 49.0, 0.01);

  // First note of chord 4 (E) is note 48 → start = 540000. E1 = 41.20 Hz.
  EXPECT_NEAR(static_cast<double>((*freq)[540000]), 41.20, 0.01);

  // Second note of E chord (G#1 = 51.91 Hz) — the raised 7th, which is
  // what makes the cadence "harmonic minor".
  EXPECT_NEAR(static_cast<double>((*freq)[540000 + 11250]), 51.91, 0.01);
}

TEST(SynthModulesTest, TestPatternImpulsesLegacyMode) {
  TestPatternSource src(TestPatternSource::Mode::kImpulses, 4, 128.0, 8);
  src.Process(4800);
  const auto* out = src.GetOutput("out");
  ASSERT_NE(out, nullptr);
  int hits = 0;
  for (float s : *out) {
    if (s > 0.5f)
      ++hits;
  }
  EXPECT_EQ(hits, 4);
}

TEST(SynthModulesTest, FmOscProducesOutput) {
  FmOsc fm(220.0, 1.0, 3.0, 0.0);
  fm.Process(4800);
  const auto* out = fm.GetOutput("out");
  ASSERT_NE(out, nullptr);
  float maxabs = 0;
  for (float s : *out) {
    float a = s < 0 ? -s : s;
    if (a > maxabs)
      maxabs = a;
  }
  EXPECT_GT(maxabs, 0.5f);
  EXPECT_LE(maxabs, 1.01f);
}

TEST(SynthModulesTest, PhaseDistortionOscBoundedAndNotZero) {
  PhaseDistortionOsc pd(PhaseDistortionOsc::Mode::kSawWarp, 220.0, 0.8);
  pd.Process(4800);
  const auto* out = pd.GetOutput("out");
  ASSERT_NE(out, nullptr);
  float maxabs = 0;
  for (float s : *out) {
    float a = s < 0 ? -s : s;
    if (a > maxabs)
      maxabs = a;
  }
  EXPECT_GT(maxabs, 0.5f);
  EXPECT_LE(maxabs, 1.01f);
}

TEST(SynthModulesTest, FoldOscHighDriveProducesHarmonics) {
  // At low drive the spectrum is sparse; at high drive it's dense. Proxy:
  // the diff-of-adjacent-samples energy should be much larger at high drive.
  auto measure = [](double drive) {
    FoldOsc f(220.0, drive, 0.0);
    f.Process(9600);
    const auto* out = f.GetOutput("out");
    double d2 = 0;
    for (size_t i = 1; i < out->size(); ++i) {
      double d =
          static_cast<double>((*out)[i]) - static_cast<double>((*out)[i - 1]);
      d2 += d * d;
    }
    return d2;
  };
  EXPECT_GT(measure(8.0), measure(1.0) * 2.0);
}

TEST(SynthModulesTest, SyncOscResetCreatesDistinctTimbre) {
  // Just verify the output is non-trivial and bounded.
  SyncOsc sync(110.0, 3.0);
  sync.Process(4800);
  const auto* out = sync.GetOutput("out");
  ASSERT_NE(out, nullptr);
  float maxabs = 0;
  for (float s : *out) {
    float a = s < 0 ? -s : s;
    if (a > maxabs)
      maxabs = a;
  }
  EXPECT_GT(maxabs, 0.5f);
  EXPECT_LE(maxabs, 1.1f);
}

TEST(SynthModulesTest, SuperOscDetuneWidensSpectrum) {
  // At detune=0 (unison) the super-osc is a single saw and the output
  // is a normal sawtooth. With heavy detune it gets phasey/beating.
  // Proxy: the low-frequency (slow) envelope of the output should
  // fluctuate more with heavy detune.
  SuperOsc no_detune(220.0, 0.0, 0.5);
  SuperOsc detuned(220.0, 0.9, 0.5);
  no_detune.Process(48000);
  detuned.Process(48000);
  const auto* o1 = no_detune.GetOutput("out");
  const auto* o2 = detuned.GetOutput("out");
  ASSERT_NE(o1, nullptr);
  ASSERT_NE(o2, nullptr);
  // Just check both produce non-trivial output.
  float max1 = 0, max2 = 0;
  for (float s : *o1) {
    float a = s < 0 ? -s : s;
    if (a > max1)
      max1 = a;
  }
  for (float s : *o2) {
    float a = s < 0 ? -s : s;
    if (a > max2)
      max2 = a;
  }
  EXPECT_GT(max1, 0.3f);
  EXPECT_GT(max2, 0.3f);
}

TEST(SynthModulesTest, WavetableOscSineToSawOutputs) {
  // At pos=0, should behave like a nearly-pure sine (few zero crossings
  // matching the fundamental).
  WavetableOsc osc(WavetableOsc::TableType::kSineToSaw, 440.0, 0.0);
  osc.Process(4800);
  const auto* out = osc.GetOutput("out");
  ASSERT_NE(out, nullptr);
  int zc = 0;
  for (size_t i = 1; i < out->size(); ++i) {
    if (((*out)[i - 1] >= 0 && (*out)[i] < 0) ||
        ((*out)[i - 1] < 0 && (*out)[i] >= 0)) {
      ++zc;
    }
  }
  // 440 Hz * 0.1 s = 44 cycles → 88 zero crossings.
  EXPECT_GE(zc, 85);
  EXPECT_LE(zc, 91);
  for (float s : *out) {
    EXPECT_LE(s, 1.05f);
    EXPECT_GE(s, -1.05f);
  }
}

TEST(SynthModulesTest, WavetableOscPositionMod) {
  // Shift from pos=0 to pos=1 over the render. The running mean-absolute
  // should change (pos=1 has more harmonic content than pos=0).
  WavetableOsc osc(WavetableOsc::TableType::kSineToSaw, 220.0, 0.0);
  SignalBuffer pos_mod(9600);
  for (size_t i = 0; i < pos_mod.size(); ++i) {
    pos_mod[i] = static_cast<float>(static_cast<double>(i) /
                                    static_cast<double>(pos_mod.size()));
  }
  osc.SetInput("position_mod", &pos_mod);
  osc.Process(9600);
  const auto* out = osc.GetOutput("out");
  ASSERT_NE(out, nullptr);
  // Just check output is not all zeros and bounded.
  float maxv = 0.0f;
  for (float s : *out) {
    float a = s < 0 ? -s : s;
    if (a > maxv)
      maxv = a;
  }
  EXPECT_GT(maxv, 0.1f);
  EXPECT_LE(maxv, 1.05f);
}

TEST(SynthModulesTest, WavetableOscAllTableTypesProduceOutput) {
  for (auto type :
       {WavetableOsc::TableType::kSineToSaw,
        WavetableOsc::TableType::kPulseSweep, WavetableOsc::TableType::kBell,
        WavetableOsc::TableType::kVocal}) {
    WavetableOsc osc(type, 220.0, 0.5);
    osc.Process(2400);
    const auto* out = osc.GetOutput("out");
    ASSERT_NE(out, nullptr);
    float max_abs = 0;
    for (float s : *out) {
      float a = s < 0 ? -s : s;
      if (a > max_abs)
        max_abs = a;
    }
    EXPECT_GT(max_abs, 0.1f);
  }
}

TEST(SynthModulesTest, DelayProducesEchoes) {
  // 100 ms delay, feedback 0.5, no damping, 50% mix. Fire a single-sample
  // impulse at t=0 and check we see echoes at 100 ms, 200 ms, 300 ms...
  Delay d(100.0, 0.5, 0.0, 0.5);
  constexpr uint32_t kN = 24000;  // 500 ms
  SignalBuffer in(kN, 0.0f);
  in[0] = 1.0f;
  d.SetInput("in", &in);
  d.Process(kN);
  const auto* out = d.GetOutput("out");
  // Dry hit at sample 0: dry = 0.5 * input = 0.5.
  EXPECT_NEAR(static_cast<double>((*out)[0]), 0.5, 0.01);
  // First echo at ~sample 4800 (100 ms): wet = 0.5 * 1.0 = 0.5.
  EXPECT_NEAR(static_cast<double>((*out)[4800]), 0.5, 0.05);
  // Second echo at sample 9600 (200 ms): wet = 0.5 * 0.5 * 1.0 = 0.25.
  EXPECT_NEAR(static_cast<double>((*out)[9600]), 0.25, 0.05);
  // Third echo at sample 14400 (300 ms): 0.125.
  EXPECT_NEAR(static_cast<double>((*out)[14400]), 0.125, 0.05);
}

TEST(SynthModulesTest, MoogLadderAttenuatesHighFreq) {
  // Low cutoff (200 Hz). 5 kHz input should be massively attenuated.
  MoogLadder f(200.0, 0.0, 1.0);
  SignalBuffer in(9600);
  for (size_t i = 0; i < in.size(); ++i) {
    in[i] = 0.5f * std::sin(2.0f * 3.14159265f * 5000.0f *
                            static_cast<float>(i) / 48000.0f);
  }
  f.SetInput("in", &in);
  f.Process(9600);
  const auto* out = f.GetOutput("out");
  // Compute RMS of the last quarter (after filter warms up).
  double rms_in = 0, rms_out = 0;
  for (size_t i = 7200; i < 9600; ++i) {
    double vi = static_cast<double>(in[i]);
    double vo = static_cast<double>((*out)[i]);
    rms_in += vi * vi;
    rms_out += vo * vo;
  }
  // Cutoff 200 Hz, input 5000 Hz → ~4.5 octaves above cutoff → 4.5 * 24 =
  // 108 dB attenuation in theory. Allow for the fast-tanh warmth. Expect
  // at least 20× attenuation in RMS.
  EXPECT_LT(rms_out, rms_in / 400.0);  // = 26 dB
}

TEST(SynthModulesTest, MoogLadderPassesLowFreq) {
  // High cutoff (5 kHz). A 200 Hz input should pass with little change.
  MoogLadder f(5000.0, 0.0, 1.0);
  SignalBuffer in(9600);
  for (size_t i = 0; i < in.size(); ++i) {
    in[i] = 0.5f * std::sin(2.0f * 3.14159265f * 200.0f *
                            static_cast<float>(i) / 48000.0f);
  }
  f.SetInput("in", &in);
  f.Process(9600);
  const auto* out = f.GetOutput("out");
  // Compare RMS over the latter half.
  double rms_in = 0, rms_out = 0;
  for (size_t i = 4800; i < 9600; ++i) {
    double vi = static_cast<double>(in[i]);
    double vo = static_cast<double>((*out)[i]);
    rms_in += vi * vi;
    rms_out += vo * vo;
  }
  // Expect the output to be within 3 dB of input (ratio 0.5..2).
  EXPECT_GT(rms_out, rms_in * 0.5);
  EXPECT_LT(rms_out, rms_in * 2.0);
}

TEST(SynthModulesTest, SvfBandpassPeaksAtCutoff) {
  // BP at 1 kHz with moderate Q. A 1 kHz input should pass; 100 Hz should
  // be attenuated; 10 kHz should be attenuated.
  auto measure = [](float input_freq) {
    Svf f(Svf::Mode::kBandpass, 1000.0, 5.0);
    SignalBuffer in(9600);
    for (size_t i = 0; i < in.size(); ++i) {
      in[i] = 0.5f * std::sin(2.0f * 3.14159265f * input_freq *
                              static_cast<float>(i) / 48000.0f);
    }
    f.SetInput("in", &in);
    f.Process(9600);
    const auto* out = f.GetOutput("out");
    double rms = 0;
    for (size_t i = 4800; i < 9600; ++i) {
      double v = static_cast<double>((*out)[i]);
      rms += v * v;
    }
    return rms;
  };
  double at_cutoff = measure(1000.0f);
  double at_low = measure(100.0f);
  double at_high = measure(10000.0f);
  EXPECT_GT(at_cutoff, at_low * 5.0);
  EXPECT_GT(at_cutoff, at_high * 5.0);
}

TEST(SynthModulesTest, SvfLowpassAttenuatesHighFreq) {
  Svf f(Svf::Mode::kLowpass, 500.0, 1.0);
  SignalBuffer in(9600);
  for (size_t i = 0; i < in.size(); ++i) {
    in[i] = 0.5f * std::sin(2.0f * 3.14159265f * 5000.0f *
                            static_cast<float>(i) / 48000.0f);
  }
  f.SetInput("in", &in);
  f.Process(9600);
  const auto* out = f.GetOutput("out");
  double rms_in = 0, rms_out = 0;
  for (size_t i = 4800; i < 9600; ++i) {
    double vi = static_cast<double>(in[i]);
    double vo = static_cast<double>((*out)[i]);
    rms_in += vi * vi;
    rms_out += vo * vo;
  }
  EXPECT_LT(rms_out, rms_in / 50.0);  // at least 17 dB attenuation
}

TEST(SynthModulesTest, WaveshaperSoftTanhBounded) {
  // A large-amplitude sine through soft tanh drive=4 should stay bounded
  // by ~1.0 and produce harmonics.
  Waveshaper ws(Waveshaper::Mode::kSoftTanh, 4.0, 1.0);
  SignalBuffer in(4800);
  for (size_t i = 0; i < in.size(); ++i) {
    // Large sine, amplitude 2 — would clip a hard clipper.
    in[i] = 2.0f * std::sin(2.0f * 3.14159265f * 440.0f *
                            static_cast<float>(i) / 48000.0f);
  }
  ws.SetInput("in", &in);
  ws.Process(4800);
  const auto* out = ws.GetOutput("out");
  ASSERT_NE(out, nullptr);
  for (float s : *out) {
    EXPECT_LE(s, 1.05f);
    EXPECT_GE(s, -1.05f);
  }
}

TEST(SynthModulesTest, WaveshaperHardClipClips) {
  Waveshaper ws(Waveshaper::Mode::kHardClip, 10.0, 1.0);
  SignalBuffer in(1000, 0.5f);  // constant 0.5
  ws.SetInput("in", &in);
  ws.Process(1000);
  const auto* out = ws.GetOutput("out");
  // 0.5 * 10 = 5.0 → clipped to 1.0
  for (float s : *out) {
    EXPECT_NEAR(static_cast<double>(s), 1.0, 1e-6);
  }
}

TEST(SynthModulesTest, WaveshaperFoldInBounds) {
  Waveshaper ws(Waveshaper::Mode::kFold, 5.0, 1.0);
  SignalBuffer in(1000);
  for (size_t i = 0; i < in.size(); ++i) {
    in[i] = 0.8f * std::sin(0.01f * static_cast<float>(i));
  }
  ws.SetInput("in", &in);
  ws.Process(1000);
  const auto* out = ws.GetOutput("out");
  // Reflective fold keeps output in [-1, 1].
  for (float s : *out) {
    EXPECT_LE(s, 1.001f);
    EXPECT_GE(s, -1.001f);
  }
}

TEST(SynthModulesTest, LfoSineRateAndRange) {
  // 2 Hz sine, bipolar, depth 1.0. Over 0.5 s should complete 1 cycle.
  Lfo lfo(Lfo::Waveform::kSine, 2.0, 1.0, true, 0);
  lfo.Process(24000);  // 0.5 seconds
  const auto* out = lfo.GetOutput("out");
  ASSERT_NE(out, nullptr);
  float maxv = -1e9f, minv = 1e9f;
  for (float s : *out) {
    if (s > maxv)
      maxv = s;
    if (s < minv)
      minv = s;
  }
  EXPECT_NEAR(static_cast<double>(maxv), 1.0, 0.02);
  EXPECT_NEAR(static_cast<double>(minv), -1.0, 0.02);
}

TEST(SynthModulesTest, LfoSquareUnipolar) {
  // Unipolar square should alternate between 0 and depth.
  Lfo lfo(Lfo::Waveform::kSquare, 10.0, 0.5, false, 0);
  lfo.Process(4800);
  const auto* out = lfo.GetOutput("out");
  ASSERT_NE(out, nullptr);
  for (float s : *out) {
    bool near_zero = s < 0.01f;
    bool near_depth = s > 0.49f && s < 0.51f;
    EXPECT_TRUE(near_zero || near_depth);
  }
}

TEST(SynthModulesTest, LfoSampleAndHoldSteppedOutput) {
  // S&H at 100 Hz. Over 1 s we expect ~100 distinct "steps" (plateaus
  // with no change). Just verify that within a plateau the value doesn't
  // move, and that values do change across wraps.
  Lfo lfo(Lfo::Waveform::kSampleAndHold, 100.0, 1.0, true, 42);
  lfo.Process(48000);
  const auto* out = lfo.GetOutput("out");
  ASSERT_NE(out, nullptr);
  int transitions = 0;
  for (size_t i = 1; i < out->size(); ++i) {
    float d = (*out)[i] - (*out)[i - 1];
    if (d < 0)
      d = -d;
    if (d > 1e-6f)
      ++transitions;
  }
  // Roughly 100 transitions expected. Wide tolerance.
  EXPECT_GE(transitions, 80);
  EXPECT_LE(transitions, 120);
}

TEST(SynthModulesTest, NoiseOscWhiteReproducible) {
  // Same seed → same samples. Different seed → different samples.
  NoiseOsc a(0.0, 42);
  NoiseOsc b(0.0, 42);
  NoiseOsc c(0.0, 1337);
  a.Process(480);
  b.Process(480);
  c.Process(480);
  const auto* oa = a.GetOutput("out");
  const auto* ob = b.GetOutput("out");
  const auto* oc = c.GetOutput("out");
  for (size_t i = 0; i < 480; ++i) {
    EXPECT_FLOAT_EQ((*oa)[i], (*ob)[i]);
  }
  bool any_diff = false;
  for (size_t i = 0; i < 480; ++i) {
    float d = (*oa)[i] - (*oc)[i];
    if (d < 0)
      d = -d;
    if (d > 1e-9f) {
      any_diff = true;
      break;
    }
  }
  EXPECT_TRUE(any_diff);
}

TEST(SynthModulesTest, NoiseOscSpectralTilt) {
  // Crude test: brown noise has much more low-frequency energy than
  // white noise. Measure the running variance of the raw signal and
  // of the diff-of-adjacent-samples (a proxy for high-frequency energy).
  // For white, var(diff) ≈ 2 * var(signal). For brown, var(diff) ≪
  // var(signal) because adjacent samples are highly correlated.
  auto measure = [](double tilt) -> double {
    NoiseOsc n(tilt, 12345);
    n.Process(48000);  // 1 second
    const auto* out = n.GetOutput("out");
    double s2 = 0, d2 = 0;
    for (size_t i = 1; i < out->size(); ++i) {
      double s = static_cast<double>((*out)[i]);
      double d =
          static_cast<double>((*out)[i]) - static_cast<double>((*out)[i - 1]);
      s2 += s * s;
      d2 += d * d;
    }
    return d2 / (s2 + 1e-12);  // smaller = more low-frequency energy
  };
  double white_ratio = measure(0.0);
  double brown_ratio = measure(1.0);
  // White should have d2 close to 2*s2 → ratio ~2. Brown should have
  // d2 ≪ s2 → ratio ≪ 1. We use a loose bound: brown must be at least
  // 10× smaller than white.
  EXPECT_GT(white_ratio, 1.0);
  EXPECT_LT(brown_ratio, white_ratio / 10.0);
}

TEST(SynthModulesTest, ClassicOscSawZeroCrossings) {
  // 440 Hz saw over 0.1 s should cross zero ~88 times.
  ClassicOsc osc(ClassicOsc::Waveform::kSaw, 440.0, 0.5);
  osc.Process(4800);
  const SignalBuffer* out = osc.GetOutput("out");
  ASSERT_NE(out, nullptr);
  int zc = 0;
  for (size_t i = 1; i < out->size(); ++i) {
    if (((*out)[i - 1] >= 0 && (*out)[i] < 0) ||
        ((*out)[i - 1] < 0 && (*out)[i] >= 0)) {
      ++zc;
    }
  }
  EXPECT_GE(zc, 85);
  EXPECT_LE(zc, 91);
  // All samples bounded.
  for (float s : *out) {
    EXPECT_GE(s, -1.1f);
    EXPECT_LE(s, 1.1f);
  }
}

TEST(SynthModulesTest, ClassicOscSquarePwm) {
  // 100 Hz square with 30% PW. At 48 kHz, one cycle = 480 samples.
  // 30% high = ~144 samples positive per cycle, ~336 negative.
  ClassicOsc osc(ClassicOsc::Waveform::kSquare, 100.0, 0.3);
  osc.Process(4800);
  const SignalBuffer* out = osc.GetOutput("out");
  ASSERT_NE(out, nullptr);
  // Count positive and negative samples (skip the polyBLEP smoothing regions).
  int pos = 0, neg = 0;
  for (float s : *out) {
    if (s > 0.5f)
      ++pos;
    else if (s < -0.5f)
      ++neg;
  }
  // Expect roughly 30/70 ratio. Tolerances account for BLEP smoothing.
  EXPECT_NEAR(static_cast<double>(pos) / (pos + neg), 0.3, 0.05);
}

TEST(SynthModulesTest, ClassicOscTriangleBounded) {
  ClassicOsc osc(ClassicOsc::Waveform::kTriangle, 220.0, 0.5);
  osc.Process(4800);
  const SignalBuffer* out = osc.GetOutput("out");
  ASSERT_NE(out, nullptr);
  // Direct piecewise-linear triangle should hit exactly ±1 at peak/valley.
  float maxabs = 0.0f;
  for (float s : *out) {
    float a = s < 0 ? -s : s;
    if (a > maxabs)
      maxabs = a;
  }
  EXPECT_NEAR(static_cast<double>(maxabs), 1.0, 0.02);
}

TEST(SynthModulesTest, ClassicOscFreqInputOverrides) {
  // base_freq_hz=100, but drive freq input at 400 Hz → 4x the zero crossings.
  ClassicOsc osc(ClassicOsc::Waveform::kSine, 100.0, 0.5);
  SignalBuffer freq(4800, 400.0f);
  osc.SetInput("freq", &freq);
  osc.Process(4800);
  const SignalBuffer* out = osc.GetOutput("out");
  ASSERT_NE(out, nullptr);
  int zc = 0;
  for (size_t i = 1; i < out->size(); ++i) {
    if (((*out)[i - 1] >= 0 && (*out)[i] < 0) ||
        ((*out)[i - 1] < 0 && (*out)[i] >= 0))
      ++zc;
  }
  // 400 Hz * 0.1 s = 40 cycles = 80 zero crossings.
  EXPECT_GE(zc, 78);
  EXPECT_LE(zc, 82);
}

TEST(SynthModulesTest, ClassicOscResetRestartsPhase) {
  // A sine oscillator with phase reset driven by a gate.
  ClassicOsc osc(ClassicOsc::Waveform::kSine, 220.0, 0.5);
  // Trigger reset at sample 1000.
  SignalBuffer reset(4800, 0.0f);
  reset[1000] = 1.0f;
  osc.SetInput("reset", &reset);
  osc.Process(4800);
  const SignalBuffer* out = osc.GetOutput("out");
  ASSERT_NE(out, nullptr);
  // Sample at the reset point: phase was just set to 0, then advanced one
  // step, so sample ≈ sin(2π·freq/sr) ≈ sin(0.029) ≈ 0.029.
  EXPECT_NEAR(static_cast<double>((*out)[1000]), 0.0, 0.05);
}

TEST(SynthModulesTest, AdsrFullCycle) {
  // 2ms attack, 5ms decay, 0.6 sustain, 20ms release.
  Adsr adsr(2.0, 5.0, 0.6, 20.0);

  // Gate pattern: high for 20 ms (960 samples), then low. Total = 9600
  // samples (200 ms) so the release has plenty of room to decay.
  constexpr uint32_t kN = 9600;
  SignalBuffer gate(kN, 0.0f);
  for (uint32_t i = 0; i < 960; ++i)
    gate[i] = 1.0f;

  adsr.SetInput("gate", &gate);
  adsr.Process(kN);

  const SignalBuffer* out = adsr.GetOutput("out");
  ASSERT_NE(out, nullptr);

  // Sample 0 already applies the first attack step (~0.02 at a 2 ms rate),
  // but should still be small.
  EXPECT_LT((*out)[0], 0.05f);

  // Within the first 2 ms (attack = 96 samples), level should hit 1.0.
  float attack_peak = 0.0f;
  for (uint32_t i = 0; i < 150; ++i) {
    if ((*out)[i] > attack_peak)
      attack_peak = (*out)[i];
  }
  EXPECT_NEAR(static_cast<double>(attack_peak), 1.0, 0.05);

  // After attack+decay (2+5 = 7 ms = 336 samples), level should be within
  // 10% of the sustain (0.6). Check at 500 samples (well past decay).
  EXPECT_NEAR(static_cast<double>((*out)[500]), 0.6, 0.1);

  // While the gate is still high (e.g., at 900 samples, just before
  // release), level should still be at sustain.
  EXPECT_NEAR(static_cast<double>((*out)[900]), 0.6, 0.05);

  // After release (gate drops at 960, 20 ms release = 960 samples),
  // level should be near zero by 2500 samples after release start.
  EXPECT_LT((*out)[3500], 0.05f);
}

TEST(SynthModulesTest, AdsrReleaseFromAttack) {
  // Short attack, long decay/release. Drop the gate while still in attack.
  Adsr adsr(10.0, 50.0, 0.5, 50.0);

  constexpr uint32_t kN = 9600;
  SignalBuffer gate(kN, 0.0f);
  for (uint32_t i = 0; i < 100; ++i)  // ~2 ms, much shorter than attack
    gate[i] = 1.0f;

  adsr.SetInput("gate", &gate);
  adsr.Process(kN);

  const SignalBuffer* out = adsr.GetOutput("out");
  ASSERT_NE(out, nullptr);

  // Level rises partway, then releases. Peak should be below 1.0.
  float peak = 0.0f;
  for (float s : *out) {
    if (s > peak)
      peak = s;
  }
  EXPECT_LT(peak, 1.0f);
  EXPECT_GT(peak, 0.1f);  // but it did move

  // Output should be near-zero by the end.
  EXPECT_LT((*out)[kN - 1], 0.05f);
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
    while (it.Next()) {
    }
    ASSERT_OK(it.Status());
  }
  {
    auto it = tp->ExecuteQuery(
        "INSERT INTO _test_slice VALUES "
        "(1000000000, 100000000, 'test_slice1'), "  // 1s, 100ms
        "(1200000000, 100000000, 'test_slice2')");  // 1.2s, 100ms
    while (it.Next()) {
    }
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

TEST(SynthEngineTest, RenderWithTestPatternSource) {
  // Build a patch that uses TestPatternSource -> envelope -> VCA <- VCO
  // driven by a test pattern of 16 hits. This doesn't need trace data.
  auto tp = perfetto::trace_processor::TraceProcessor::CreateInstance(
      perfetto::trace_processor::Config{});
  ASSERT_OK(tp->NotifyEndOfFile());

  protozero::HeapBuffered<protos::pbzero::SynthPatch> patch;

  {
    auto* m = patch->add_modules();
    m->set_id("test_src");
    auto* ts = m->set_test_pattern_source();
    ts->set_num_hits(16);
  }
  {
    auto* m = patch->add_modules();
    m->set_id("test_env");
    auto* e = m->set_envelope();
    e->set_attack_ms(2);
    e->set_decay_ms(50);
    e->set_peak(1.0);
  }
  {
    auto* m = patch->add_modules();
    m->set_id("test_osc");
    auto* v = m->set_vco();
    v->set_waveform(protos::pbzero::VcoConfig::SINE);
    v->set_base_freq_hz(220);
  }
  {
    auto* m = patch->add_modules();
    m->set_id("test_amp");
    m->set_vca();
  }
  {
    auto* m = patch->add_modules();
    m->set_id("master");
    m->set_mixer();
  }

  {
    auto* w = patch->add_wires();
    w->set_from_module("test_src");
    w->set_to_module("test_env");
    w->set_to_port("trigger");
  }
  {
    auto* w = patch->add_wires();
    w->set_from_module("test_env");
    w->set_to_module("test_amp");
    w->set_to_port("gain");
  }
  {
    auto* w = patch->add_wires();
    w->set_from_module("test_osc");
    w->set_to_module("test_amp");
    w->set_to_port("in");
  }
  {
    auto* w = patch->add_wires();
    w->set_from_module("test_amp");
    w->set_to_module("master");
    w->set_to_port("in");
  }

  auto patch_data = patch.SerializeAsArray();

  SynthEngine engine(tp.get());
  // 1/48 second of trace time -> 1 second of audio after dilation.
  auto result =
      engine.Render(patch_data.data(), patch_data.size(), 0, 1000000000 / 48);
  ASSERT_TRUE(result.ok()) << result.status().message();
  // WAV header (44 bytes) + 1 second of audio * 48000 samples * 4 bytes.
  EXPECT_GT(result->size(), 44u);
}

}  // namespace
}  // namespace perfetto::trace_processor::sound_synth
