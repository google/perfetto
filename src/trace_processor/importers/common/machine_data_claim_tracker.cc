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
#include <map>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/global_stats_tracker.h"
#include "src/trace_processor/importers/common/import_logs_tracker.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/tables/sched_tables_py.h"
#include "src/trace_processor/tables/slice_tables_py.h"
#include "src/trace_processor/types/trace_manifest_state.h"
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
bool MachineDataClaimTracker::IsEnabled(const TraceProcessorContext* context) {
  if (!context) {
    return false;
  }
  if (context->config.drop_duplicate_machine_data ==
      Config::DropDuplicateMachineData::kOn) {
    return true;
  }
  if (context->config.drop_duplicate_machine_data ==
      Config::DropDuplicateMachineData::kOff) {
    return false;
  }
  if (context->trace_manifest_state &&
      context->trace_manifest_state->drop_duplicate_machine_data.has_value()) {
    return *context->trace_manifest_state->drop_duplicate_machine_data;
  }
  return false;
}

// static
std::optional<MachineDataClaimTracker::Kind>
MachineDataClaimTracker::KindForTracePacketField(uint32_t field_id) {
  using protos::pbzero::TracePacket;
  switch (field_id) {
    case TracePacket::kGenericKernelTaskStateEventFieldNumber:
    case TracePacket::kGenericKernelTaskRenameEventFieldNumber:
      return Kind::kSched;
    case TracePacket::kGenericKernelCpuFreqEventFieldNumber:
    case TracePacket::kGenericGpuFrequencyEventFieldNumber:
      return Kind::kCounter;
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
    case Kind::kSched:
      return "sched";
    case Kind::kFtrace:
      return "ftrace";
    case Kind::kSysStats:
      return "sys_stats";
    case Kind::kCpuPerUid:
      return "cpu_per_uid";
    case Kind::kAndroidPower:
      return "android_power";
    case Kind::kCounter:
      return "counter";
  }
  PERFETTO_FATAL("For GCC");
}

// static
size_t MachineDataClaimTracker::StatForKind(Kind kind) {
  switch (kind) {
    case Kind::kSched:
      return stats::machine_sched_claim_conflict;
    case Kind::kFtrace:
      return stats::machine_ftrace_claim_conflict;
    case Kind::kSysStats:
      return stats::machine_sys_stats_claim_conflict;
    case Kind::kCpuPerUid:
      return stats::machine_cpu_per_uid_claim_conflict;
    case Kind::kAndroidPower:
      return stats::machine_android_power_claim_conflict;
    case Kind::kCounter:
      return stats::machine_counter_claim_conflict;
  }
  PERFETTO_FATAL("For GCC");
}

void MachineDataClaimTracker::RegisterMachineTrack(TrackId track_id,
                                                   std::string_view) {
  if (track_id.value >= track_is_machine_.size()) {
    track_is_machine_.resize(track_id.value + 1, false);
  }
  track_is_machine_[track_id.value] = true;
  shared_track_kinds_.Insert(track_id, Kind::kCounter);
}

// static
bool MachineDataClaimTracker::KeepCounter(TraceProcessorContext* caller,
                                          TrackId track,
                                          int64_t ts) {
  if (!IsEnabled(caller)) {
    return true;
  }
  auto* self = caller->machine_data_claim_tracker.get();
  if (!self) {
    return true;
  }
  if (track.value >= self->track_is_machine_.size() ||
      !self->track_is_machine_[track.value]) {
    return true;
  }
  return self->ShouldImport(caller, Kind::kCounter, ts);
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
  size_t stat_key = StatForKind(kind);
  auto it = std::find_if(
      claims.drops.begin(), claims.drops.end(),
      [trace_id](const Drops& d) { return d.trace_id == trace_id; });
  if (it != claims.drops.end()) {
    it->count++;
    context->stats_tracker->IncrementStats(stat_key);
    return;
  }
  claims.drops.push_back(Drops{trace_id, 1});

  // The first drop of this kind from this trace: explain it. This also
  // increments the stat.
  StringId kind_name = kind_names_[static_cast<size_t>(kind)];
  context->import_logs_tracker->RecordParserLog(
      stat_key, ts,
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
  if (!IsEnabled(context_)) {
    return;
  }
  auto merged = [this](Kind kind) {
    return claims_[static_cast<size_t>(kind)].windows.size() > 1;
  };
  if (merged(Kind::kSched) || merged(Kind::kFtrace)) {
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
  // Within each followed window and CPU, only close the latest open slice
  // (the one at the trace boundary). Earlier dur == -1 rows are left alone.
  std::map<std::pair<const Window*, uint32_t>, uint32_t> latest_open;
  for (uint32_t i = 0; i < sched->row_count(); ++i) {
    auto r = (*sched)[i];
    if (r.dur() != -1 ||
        threads[r.utid()].machine_id() != context_->machine_id()) {
      continue;
    }
    const Window* window = FollowedWindowContaining(Kind::kSched, r.ts());
    if (!window) {
      window = FollowedWindowContaining(Kind::kFtrace, r.ts());
    }
    if (!window) {
      continue;
    }
    uint32_t ucpu = sched->dataframe().GetCellUnchecked<
        tables::SchedSliceTable::ColumnIndex::ucpu>(
        tables::SchedSliceTable::kSpec, i);
    auto key = std::make_pair(window, ucpu);
    auto it = latest_open.find(key);
    if (it == latest_open.end() || (*sched)[it->second].ts() < r.ts()) {
      latest_open[key] = i;
    }
  }

  for (const auto& [key, row_idx] : latest_open) {
    auto r = (*sched)[row_idx];
    const Window* window = key.first;
    context_->global_stats_tracker->IncrementStats(
        context_->machine_id(), window->trace_id,
        stats::machine_data_closed_at_trace_boundary);
    r.set_dur(window->max - r.ts());
  }
}

void MachineDataClaimTracker::CloseOpenThreadStates() {
  const auto& threads = context_->storage->thread_table();
  auto* states = context_->storage->mutable_thread_state_table();
  // Within each followed window and utid, only close the latest open state
  // (the one at the trace boundary). Earlier dur == -1 rows are left alone.
  std::map<std::pair<const Window*, UniqueTid>, uint32_t> latest_open;
  for (uint32_t i = 0; i < states->row_count(); ++i) {
    auto r = (*states)[i];
    if (r.dur() != -1 ||
        threads[r.utid()].machine_id() != context_->machine_id()) {
      continue;
    }
    const Window* window = FollowedWindowContaining(Kind::kSched, r.ts());
    if (!window) {
      window = FollowedWindowContaining(Kind::kFtrace, r.ts());
    }
    if (!window) {
      continue;
    }
    auto key = std::make_pair(window, r.utid());
    auto it = latest_open.find(key);
    if (it == latest_open.end() || (*states)[it->second].ts() < r.ts()) {
      latest_open[key] = i;
    }
  }

  for (const auto& [key, row_idx] : latest_open) {
    auto r = (*states)[row_idx];
    const Window* window = key.first;
    context_->global_stats_tracker->IncrementStats(
        context_->machine_id(), window->trace_id,
        stats::machine_data_closed_at_trace_boundary);
    r.set_dur(window->max - r.ts());
  }
}

void MachineDataClaimTracker::CloseOpenSlicesOnSharedTracks() {
  auto* slices = context_->storage->mutable_slice_table();
  // Within each followed window and track, only close the latest open slice
  // (the one at the trace boundary). Earlier dur == -1 rows are left alone.
  std::map<std::pair<const Window*, TrackId>, uint32_t> latest_open;
  for (uint32_t i = 0; i < slices->row_count(); ++i) {
    auto r = (*slices)[i];
    if (r.dur() != -1) {
      continue;
    }
    Kind* kind = shared_track_kinds_.Find(r.track_id());
    if (!kind) {
      continue;
    }
    const Window* window = FollowedWindowContaining(*kind, r.ts());
    if (!window) {
      continue;
    }
    auto key = std::make_pair(window, r.track_id());
    auto it = latest_open.find(key);
    if (it == latest_open.end() || (*slices)[it->second].ts() < r.ts()) {
      latest_open[key] = i;
    }
  }

  for (const auto& [key, row_idx] : latest_open) {
    auto r = (*slices)[row_idx];
    const Window* window = key.first;
    context_->global_stats_tracker->IncrementStats(
        context_->machine_id(), window->trace_id,
        stats::machine_data_closed_at_trace_boundary);
    r.set_dur(window->max - r.ts());
  }
}

}  // namespace perfetto::trace_processor
