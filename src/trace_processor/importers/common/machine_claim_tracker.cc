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

#include "src/trace_processor/importers/common/machine_claim_tracker.h"

#include <algorithm>
#include <cstdint>
#include <optional>

#include "perfetto/base/logging.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/import_logs_tracker.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"

namespace perfetto::trace_processor {
namespace {

// The value at |t| on the line through (t0, v0) and (t1, v1); t0 <= t < t1.
double Interpolate(int64_t t0, double v0, int64_t t1, double v1, int64_t t) {
  PERFETTO_DCHECK(t0 <= t && t < t1);
  return v0 + (v1 - v0) *
                  (static_cast<double>(t - t0) / static_cast<double>(t1 - t0));
}

}  // namespace

MachineClaimTracker::MachineClaimTracker(TraceProcessorContext* context)
    : track_id_key_(context->storage->InternString("track_id")),
      owner_trace_id_key_(context->storage->InternString("owner_trace_id")) {}

MachineClaimTracker::~MachineClaimTracker() = default;

// static
void MachineClaimTracker::ReadTraceEnds(TraceProcessorContext* context) {
  auto& contexts = context->forked_context_state->trace_and_machine_to_context;
  for (auto it = contexts.GetIterator(); it; ++it) {
    MachineClaimTracker* self = it.value()->machine_claim_tracker.get();
    if (!self || !self->has_multiple_traces_ || self->ends_read_) {
      continue;
    }
    self->ends_read_ = true;
    for (TraceId trace : self->traces_) {
      std::optional<int64_t> end = context->sorter->MaxTimestampForTrace(trace);
      if (end) {
        self->SetTraceEnd(trace, *end);
      }
    }
  }
}

void MachineClaimTracker::AddTrack(uint64_t hash, TrackId track) {
  tracks_.Insert(hash, track);
  if (track.value >= owners_.size()) {
    owners_.resize(track.value + 1, kNotMachineTrack);
  }
  owners_[track.value] = kNoOwner;
}

void MachineClaimTracker::AddTrace(TraceId trace) {
  if (std::find(traces_.begin(), traces_.end(), trace) == traces_.end()) {
    traces_.push_back(trace);
  }
  has_multiple_traces_ = traces_.size() > 1;
}

void MachineClaimTracker::SetTraceEnd(TraceId trace, int64_t end) {
  if (trace.value >= trace_ends_.size()) {
    trace_ends_.resize(trace.value + 1, kUnknownEnd);
  }
  trace_ends_[trace.value] = end;
}

int64_t MachineClaimTracker::TraceEnd(uint32_t trace) const {
  return trace < trace_ends_.size() ? trace_ends_[trace] : kUnknownEnd;
}

MachineClaimTracker::Claim MachineClaimTracker::Decide(uint32_t owner,
                                                       TraceId trace,
                                                       int64_t ts) const {
  if (owner == trace.value || owner == kNotMachineTrack) {
    return Claim::kOwned;
  }
  if (owner == kNoOwner) {
    return Claim::kFirstWrite;
  }
  // The owner holds the track up to and including its end.
  return ts > TraceEnd(owner) ? Claim::kHandover : Claim::kHidden;
}

MachineClaimTracker::Claim MachineClaimTracker::Check(TraceId trace,
                                                      TrackId track,
                                                      int64_t ts) const {
  // Tracks past the end are not machine-scoped (this includes
  // kInvalidTrackId).
  if (track.value >= owners_.size()) {
    return Claim::kOwned;
  }
  return Decide(owners_[track.value], trace, ts);
}

bool MachineClaimTracker::Keep(TraceProcessorContext* writer,
                               TrackId track,
                               int64_t ts) {
  if (track.value >= owners_.size()) {
    return true;
  }
  uint32_t& owner = owners_[track.value];
  TraceId trace = writer->trace_id();
  switch (Decide(owner, trace, ts)) {
    case Claim::kOwned:
      return true;
    case Claim::kFirstWrite:
    case Claim::kHandover:
      owner = trace.value;
      return true;
    case Claim::kHidden:
      RecordHidden(writer, track, ts, owner);
      return false;
  }
  PERFETTO_FATAL("For GCC");
}

void MachineClaimTracker::RecordHidden(TraceProcessorContext* writer,
                                       TrackId track,
                                       int64_t ts,
                                       uint32_t owner) {
  auto index = static_cast<int>(track.value);
  uint64_t key = (uint64_t{writer->trace_id().value} << 32) | track.value;
  bool first = logged_hidden_.Insert(key, true).second;
  if (first && writer->import_logs_tracker) {
    writer->import_logs_tracker->RecordIndexedParserLog(
        stats::machine_track_events_hidden, index, ts,
        [this, track, owner](ArgsTracker::BoundInserter& inserter) {
          inserter.AddArg(track_id_key_, Variadic::Integer(track.value));
          inserter.AddArg(owner_trace_id_key_, Variadic::Integer(owner));
        });
    return;
  }
  writer->stats_tracker->IncrementIndexedStats(
      stats::machine_track_events_hidden, index);
}

double MachineClaimTracker::SeriesValue(TraceId trace,
                                        TrackId track,
                                        int64_t ts,
                                        double value,
                                        SeriesInput* input) {
  Claim claim = Check(trace, track, ts);
  SeriesTail& tail = series_tails_[track.value];
  switch (claim) {
    case Claim::kFirstWrite:
      tail.offset = 0;
      break;
    case Claim::kHandover: {
      // The trace's own value at the old owner's last sample. A trace that
      // had a sample after it computed that value then (below); otherwise all
      // its samples so far are at or before it.
      double base = value;
      if (input->has_interp && input->interp_ts == tail.ts) {
        base = input->interp_value;
      } else if (input->has_prev) {
        base =
            Interpolate(input->prev_ts, input->prev_value, ts, value, tail.ts);
      }
      tail.offset = tail.value - base;
      break;
    }
    case Claim::kOwned:
      break;
    case Claim::kHidden:
      // On the trace's first sample after the owner's last one, store the
      // trace's value at that time, in case it takes over later.
      if (tail.ts < ts && (!input->has_prev || input->prev_ts <= tail.ts)) {
        input->interp_value =
            input->has_prev ? Interpolate(input->prev_ts, input->prev_value, ts,
                                          value, tail.ts)
                            : value;
        input->interp_ts = tail.ts;
        input->has_interp = true;
      }
      break;
  }
  input->prev_ts = ts;
  input->prev_value = value;
  input->has_prev = true;
  if (claim == Claim::kHidden) {
    return value;
  }
  double series = value + tail.offset;
  tail.ts = ts;
  tail.value = series;
  return series;
}

}  // namespace perfetto::trace_processor
