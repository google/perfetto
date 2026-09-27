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

#include "src/trace_processor/importers/common/machine_data_claim_tracker.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/import_logs_tracker.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/tables/sched_tables_py.h"
#include "src/trace_processor/tables/slice_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_processor {

MachineDataClaimTracker::MachineDataClaimTracker(TraceProcessorContext* context)
    : context_(context),
      kind_key_id_(context->storage->InternString("kind")),
      conflicting_trace_id_key_id_(
          context->storage->InternString("conflicting_trace_id")) {
  for (size_t i = 0; i < kKindCount; ++i) {
    kind_names_[i] =
        context->storage->InternString(KindToString(static_cast<Kind>(i)));
  }
}

MachineDataClaimTracker::~MachineDataClaimTracker() = default;

// static
std::optional<MachineDataClaimTracker::Kind>
MachineDataClaimTracker::KindForTracePacketField(uint32_t field_id) {
  using protos::pbzero::TracePacket;
  switch (field_id) {
    case TracePacket::kGenericKernelTaskStateEventFieldNumber:
    case TracePacket::kGenericKernelCpuFreqEventFieldNumber:
    case TracePacket::kGenericKernelTaskRenameEventFieldNumber:
    case TracePacket::kGenericGpuFrequencyEventFieldNumber:
      return Kind::kKernel;
    case TracePacket::kSysStatsFieldNumber:
      return Kind::kSysStats;
    case TracePacket::kCpuPerUidDataFieldNumber:
      return Kind::kCpuPerUid;
    case TracePacket::kBatteryFieldNumber:
    case TracePacket::kPowerRailsFieldNumber:
    case TracePacket::kEntityStateResidencyFieldNumber:
    case TracePacket::kAndroidEnergyEstimationBreakdownFieldNumber:
      return Kind::kAndroidPower;
    default:
      return std::nullopt;
  }
}

// static
const char* MachineDataClaimTracker::KindToString(Kind kind) {
  switch (kind) {
    case Kind::kKernel:
      return "kernel";
    case Kind::kSysStats:
      return "sys_stats";
    case Kind::kCpuPerUid:
      return "cpu_per_uid";
    case Kind::kAndroidPower:
      return "android_power";
  }
  PERFETTO_FATAL("For GCC");
}

bool MachineDataClaimTracker::ShouldImportSlow(TraceProcessorContext* context,
                                               Kind kind,
                                               int64_t ts) {
  PERFETTO_DCHECK(context->machine_id() == context_->machine_id());
  Claims& claims = claims_[static_cast<size_t>(kind)];
  std::vector<Window>& windows = claims.windows;
  const TraceId trace_id = context->trace_id();

  // The window which contains |ts|, if any.
  auto containing = std::find_if(
      windows.begin(), windows.end(),
      [ts](const Window& w) { return ts >= w.min && ts <= w.max; });

  auto own = std::find_if(
      windows.begin(), windows.end(),
      [trace_id](const Window& w) { return w.trace_id == trace_id; });
  if (own != windows.end()) {
    if (ts >= own->lo && ts <= own->hi) {
      own->Extend(ts);
      claims.last_accepted = static_cast<size_t>(own - windows.begin());
      return true;
    }
    // Outside the trace's window: either inside another trace's window or
    // beyond one (a trace only gets a single contiguous window). Blame the
    // window in the way: the nearest one between this trace's window and
    // |ts| or, if there is none, the window which claimed the kind after
    // this trace and so froze its window.
    std::optional<TraceId> conflicting;
    if (containing != windows.end()) {
      conflicting = containing->trace_id;
    } else {
      const Window* nearest = nullptr;
      for (const Window& w : windows) {
        bool between = ts > own->max ? (w.min > own->max && w.max < ts)
                                     : (w.max < own->min && w.min > ts);
        if (!between) {
          continue;
        }
        if (!nearest ||
            (ts > own->max ? w.min < nearest->min : w.max > nearest->max)) {
          nearest = &w;
        }
      }
      if (nearest) {
        conflicting = nearest->trace_id;
      } else if (&windows.back() != &*own) {
        conflicting = windows.back().trace_id;
      }
    }
    RecordDrop(context, kind, ts, conflicting);
    return false;
  }

  // The first data of this kind from this trace.
  if (!windows.empty() && IsExclusive(kind)) {
    RecordDrop(context, kind, ts, windows.front().trace_id);
    return false;
  }
  if (containing != windows.end()) {
    RecordDrop(context, kind, ts, containing->trace_id);
    return false;
  }

  // |ts| is in a gap between the windows of the traces before this one (or
  // before/after all of them): this trace claims the gap. From now on, only
  // this trace's window can grow, up to the windows around it.
  int64_t lo = std::numeric_limits<int64_t>::min();
  int64_t hi = std::numeric_limits<int64_t>::max();
  for (Window& w : windows) {
    w.Freeze();
    if (w.max < ts) {
      lo = std::max(lo, w.max + 1);
    } else {
      PERFETTO_DCHECK(w.min > ts);
      hi = std::min(hi, w.min - 1);
    }
  }
  windows.push_back(Window{trace_id, ts, ts, lo, hi});
  claims.last_accepted = windows.size() - 1;
  return true;
}

void MachineDataClaimTracker::RecordDrop(
    TraceProcessorContext* context,
    Kind kind,
    int64_t ts,
    std::optional<TraceId> conflicting_trace) {
  Claims& claims = claims_[static_cast<size_t>(kind)];
  TraceId trace_id = context->trace_id();
  auto it = std::find_if(
      claims.drops.begin(), claims.drops.end(),
      [trace_id](const Drops& d) { return d.trace_id == trace_id; });
  if (it != claims.drops.end()) {
    it->count++;
    context->stats_tracker->IncrementStats(
        stats::machine_data_claimed_by_other_trace);
    return;
  }
  claims.drops.push_back(Drops{trace_id, 1});

  // The first drop of this kind from this trace: explain it. This also
  // increments the stat.
  StringId kind_name = kind_names_[static_cast<size_t>(kind)];
  context->import_logs_tracker->RecordParserLog(
      stats::machine_data_claimed_by_other_trace, ts,
      [this, kind_name,
       conflicting_trace](ArgsTracker::BoundInserter& inserter) {
        inserter.AddArg(kind_key_id_, Variadic::String(kind_name));
        if (conflicting_trace) {
          inserter.AddArg(conflicting_trace_id_key_id_,
                          Variadic::Integer(
                              static_cast<int64_t>(conflicting_trace->value)));
        }
      });
}

std::vector<MachineDataClaimTracker::WindowForTesting>
MachineDataClaimTracker::GetWindowsForTesting(Kind kind) const {
  std::vector<WindowForTesting> res;
  for (const Window& w : claims_[static_cast<size_t>(kind)].windows) {
    res.push_back(WindowForTesting{w.trace_id, w.min, w.max});
  }
  return res;
}

const MachineDataClaimTracker::Window*
MachineDataClaimTracker::FollowedWindowContaining(Kind kind, int64_t ts) const {
  const std::vector<Window>& windows =
      claims_[static_cast<size_t>(kind)].windows;
  auto containing = std::find_if(
      windows.begin(), windows.end(),
      [ts](const Window& w) { return ts >= w.min && ts <= w.max; });
  if (containing == windows.end()) {
    return nullptr;
  }
  int64_t end = containing->max;
  bool followed = std::any_of(windows.begin(), windows.end(),
                              [end](const Window& w) { return w.min > end; });
  return followed ? &*containing : nullptr;
}

std::optional<int64_t> MachineDataClaimTracker::CloseAtWindowEnd(Kind kind,
                                                                 int64_t ts) {
  const Window* window = FollowedWindowContaining(kind, ts);
  if (!window) {
    return std::nullopt;
  }
  context_->global_stats_tracker->IncrementStats(
      context_->machine_id(), window->trace_id,
      stats::machine_data_closed_at_trace_boundary);
  return window->max - ts;
}

void MachineDataClaimTracker::OnEventsFullyExtracted() {
  // Nothing was ever open across a window boundary unless some kind was
  // provided by more than one trace: this keeps the common single-trace
  // import unchanged (and free).
  auto merged = [this](Kind kind) {
    return claims_[static_cast<size_t>(kind)].windows.size() > 1;
  };
  if (merged(Kind::kKernel)) {
    CloseOpenSchedSlices();
    CloseOpenThreadStates();
  }
  bool any_merged = false;
  for (size_t i = 0; i < kKindCount; ++i) {
    any_merged |= merged(static_cast<Kind>(i));
  }
  if (any_merged) {
    CloseOpenSlicesOnSharedTracks();
  }
}

void MachineDataClaimTracker::CloseOpenSchedSlices() {
  const auto& threads = context_->storage->thread_table();
  auto* sched = context_->storage->mutable_sched_slice_table();
  for (uint32_t i = 0; i < sched->row_count(); ++i) {
    auto r = (*sched)[i];
    if (r.dur() != -1 ||
        threads[r.utid()].machine_id() != context_->machine_id()) {
      continue;
    }
    if (auto dur = CloseAtWindowEnd(Kind::kKernel, r.ts()); dur) {
      r.set_dur(*dur);
    }
  }
}

void MachineDataClaimTracker::CloseOpenThreadStates() {
  const auto& threads = context_->storage->thread_table();
  auto* states = context_->storage->mutable_thread_state_table();
  for (uint32_t i = 0; i < states->row_count(); ++i) {
    auto r = (*states)[i];
    if (r.dur() != -1 ||
        threads[r.utid()].machine_id() != context_->machine_id()) {
      continue;
    }
    if (auto dur = CloseAtWindowEnd(Kind::kKernel, r.ts()); dur) {
      r.set_dur(*dur);
    }
  }
}

void MachineDataClaimTracker::CloseOpenSlicesOnSharedTracks() {
  auto* slices = context_->storage->mutable_slice_table();
  for (uint32_t i = 0; i < slices->row_count(); ++i) {
    auto r = (*slices)[i];
    if (r.dur() != -1) {
      continue;
    }
    Kind* kind = shared_track_kinds_.Find(r.track_id());
    if (!kind) {
      continue;
    }
    if (auto dur = CloseAtWindowEnd(*kind, r.ts()); dur) {
      r.set_dur(*dur);
    }
  }
}

}  // namespace perfetto::trace_processor
