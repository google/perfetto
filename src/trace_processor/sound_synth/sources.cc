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

#include "src/trace_processor/sound_synth/sources.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace perfetto::trace_processor::sound_synth {

namespace {

// A-harmonic-minor Andalusian cadence: Am - G - F - E.
// Each chord is given as 16 ascending notes (root, 3rd, 5th across 5+ octaves).
// The 4th chord uses G# (51.91, 103.83, ...) not G — that's the "harmonic
// minor" color (raised 7th degree → major V chord).
constexpr double kArpNotes[4][16] = {
    // Am (A, C, E)
    {55.00, 65.41, 82.41, 110.00, 130.81, 164.81, 220.00, 261.63, 329.63,
     440.00, 523.25, 659.25, 880.00, 1046.50, 1318.51, 1760.00},
    // G (G, B, D)
    {49.00, 61.74, 73.42, 98.00, 123.47, 146.83, 196.00, 246.94, 293.66, 392.00,
     493.88, 587.33, 783.99, 987.77, 1174.66, 1567.98},
    // F (F, A, C)
    {43.65, 55.00, 65.41, 87.31, 110.00, 130.81, 174.61, 220.00, 261.63, 349.23,
     440.00, 523.25, 698.46, 880.00, 1046.50, 1396.91},
    // E (E, G#, B)  ← G# is the raised 7th of A harmonic minor
    {41.20, 51.91, 61.74, 82.41, 103.83, 123.47, 164.81, 207.65, 246.94, 329.63,
     415.30, 493.88, 659.25, 830.61, 987.77, 1318.51},
};

// Gate duty cycle: fraction of each note duration during which the gate
// stays high. The remaining fraction is silence between notes so the
// envelope's release can play out naturally.
constexpr double kGateDuty = 0.7;

}  // namespace

// --- TraceSliceSource ---

TraceSliceSource::TraceSliceSource() {
  out_ = AddOutput("out");
}

void TraceSliceSource::Process(uint32_t /*num_samples*/) {
  // Output buffer is pre-filled by SynthEngine. Nothing to do.
}

// --- TestPatternSource ---

TestPatternSource::TestPatternSource(Mode mode,
                                     uint32_t num_hits,
                                     double bpm,
                                     uint32_t bars)
    : mode_(mode),
      num_hits_(num_hits),
      bpm_(bpm > 0 ? bpm : 128.0),
      bars_(bars > 0 ? bars : 8) {
  out_ = AddOutput("out");
  freq_ = AddOutput("freq");
}

void TestPatternSource::Process(uint32_t num_samples) {
  out_->assign(num_samples, 0.0f);
  freq_->assign(num_samples, 0.0f);
  if (num_samples == 0)
    return;

  if (mode_ == Mode::kImpulses) {
    if (num_hits_ == 0)
      return;
    for (uint32_t i = 0; i < num_hits_; ++i) {
      auto idx = static_cast<uint32_t>(
          (static_cast<uint64_t>(i) * num_samples) / num_hits_);
      if (idx < num_samples)
        (*out_)[idx] = 1.0f;
    }
    return;
  }

  // Arpeggio mode.
  //
  // Structure: `bars_` bars at `bpm_`, 8 eighth notes per bar.
  //   total_beats     = bars_ * 4
  //   total_eighths   = bars_ * 8
  //   bar_duration_s  = 4 * 60 / bpm_
  //   note_duration_s = (60 / bpm_) / 2 = 30 / bpm_
  //
  // 4 chords spread evenly across the bars. For the canonical 8-bar pattern
  // each chord gets 2 bars = 16 eighths, but we generalize so smaller/larger
  // windows work. When the total number of eighths isn't a multiple of 4,
  // the chord changes still divide evenly; at the default 8 bars we get
  // exactly 16 notes per chord.
  //
  // At 8 bars / 128 BPM that's ~15.0 seconds of content. If `num_samples` is
  // shorter, we truncate; if longer, the tail is silent.
  const double total_eighths = static_cast<double>(bars_) * 8.0;
  const double note_duration_s = 30.0 / bpm_;
  const double note_samples_d =
      note_duration_s * static_cast<double>(kSampleRate);
  const auto note_samples = static_cast<uint32_t>(note_samples_d);
  if (note_samples == 0)
    return;

  const auto total_notes = static_cast<uint32_t>(total_eighths);
  // 4 chords, `notes_per_chord` each.
  const uint32_t notes_per_chord = total_notes / 4;
  if (notes_per_chord == 0)
    return;

  for (uint32_t n = 0; n < total_notes; ++n) {
    uint32_t chord = std::min(n / notes_per_chord, 3u);
    // Distribute n-within-chord across the 16-note ascending triad. If the
    // user customized bars to give a non-16 value, we index modulo 16.
    uint32_t note_in_chord = (n - chord * notes_per_chord) %
                             (sizeof(kArpNotes[0]) / sizeof(kArpNotes[0][0]));
    double freq_hz = kArpNotes[chord][note_in_chord];

    const auto note_start = n * note_samples;
    const auto note_end = note_start + note_samples;
    const auto gate_end =
        note_start + static_cast<uint32_t>(note_samples_d * kGateDuty);
    if (note_start >= num_samples)
      break;

    // Freq held through note + gap.
    const uint32_t fr_end = std::min(note_end, num_samples);
    for (uint32_t s = note_start; s < fr_end; ++s)
      (*freq_)[s] = static_cast<float>(freq_hz);

    // Gate held high for the first 70% of the note.
    const uint32_t gt_end = std::min(gate_end, num_samples);
    for (uint32_t s = note_start; s < gt_end; ++s)
      (*out_)[s] = 1.0f;
  }
}

}  // namespace perfetto::trace_processor::sound_synth
