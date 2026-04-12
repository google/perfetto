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

#ifndef SRC_TRACE_PROCESSOR_SOUND_SYNTH_SOURCES_H_
#define SRC_TRACE_PROCESSOR_SOUND_SYNTH_SOURCES_H_

#include <cstdint>

#include "src/trace_processor/sound_synth/synth_module.h"

namespace perfetto::trace_processor::sound_synth {

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

// Generates a test pattern for previewing an instrument without trace data.
//
// Two modes:
//
//   kArpeggio (default): An 8-bar arpeggio at 128 BPM, `bars` * 8 eighth
//     notes long. The progression is i-bVII-bVI-V in A harmonic minor:
//     Am - G - F - E (Andalusian cadence; note the G# in the E chord from
//     the raised 7th). Each chord lasts `bars/4` bars, arpeggiating the
//     triad ascending across 5 octaves. Two outputs:
//       "out":  gate signal, held high for 70% of each note duration.
//       "freq": current note frequency in Hz, held through note+gap.
//
//   kImpulses: legacy mode. `num_hits` evenly-spaced single-sample impulses
//     on the "out" port. No "freq" output. Kept for backward compat.
//
class TestPatternSource : public SynthModule {
 public:
  enum class Mode { kArpeggio, kImpulses };
  TestPatternSource(Mode mode, uint32_t num_hits, double bpm, uint32_t bars);
  Type type() const override { return Type::kTestPatternSource; }
  void Process(uint32_t num_samples) override;

 private:
  Mode mode_;
  uint32_t num_hits_;
  double bpm_;
  uint32_t bars_;
  SignalBuffer* out_;   // "out" port (gate or impulses)
  SignalBuffer* freq_;  // "freq" port (arpeggio mode only; zeros in impulses)
};

}  // namespace perfetto::trace_processor::sound_synth

#endif  // SRC_TRACE_PROCESSOR_SOUND_SYNTH_SOURCES_H_
