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
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/import_logs_tracker.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/importers/common/thread_state_tracker.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"

namespace perfetto::trace_processor {

MachineDataClaimTracker::MachineDataClaimTracker(TraceProcessorContext* context)
    : context_(context),
      kind_key_id_(context->storage->InternString("kind")),
      counter_type_key_id_(context->storage->InternString("counter_type")),
      sched_kind_id_(context->storage->InternString("sched")),
      counter_kind_id_(context->storage->InternString("counter")),
      owner_trace_id_key_id_(context->storage->InternString("owner_trace_id")) {
}

MachineDataClaimTracker::~MachineDataClaimTracker() = default;

// static
std::optional<MachineDataClaimTracker::SourceKind>
MachineDataClaimTracker::SourceKindForTracePacketField(uint32_t field_id) {
  using protos::pbzero::TracePacket;
  switch (field_id) {
    case TracePacket::kSysStatsFieldNumber:
      return SourceKind::kSysStats;
    case TracePacket::kBatteryFieldNumber:
      return SourceKind::kBattery;
    case TracePacket::kEntityStateResidencyFieldNumber:
      return SourceKind::kEntityState;
    case TracePacket::kGenericKernelCpuFreqEventFieldNumber:
    case TracePacket::kGenericGpuFrequencyEventFieldNumber:
    case TracePacket::kGenericKernelProcessTreeFieldNumber:
    case TracePacket::kGenericKernelTaskStateEventFieldNumber:
    case TracePacket::kGenericKernelTaskRenameEventFieldNumber:
      return SourceKind::kGenericKernel;
    default:
      return std::nullopt;
  }
}

void MachineDataClaimTracker::OnTokenizationDone() {
  tokenization_done_ = true;
}

// static
void MachineDataClaimTracker::NotifyTokenizationDone(
    TraceProcessorContext* context) {
  if (context->forked_context_state) {
    auto& machines = context->forked_context_state->machine_to_context;
    for (auto it = machines.GetIterator(); it; ++it) {
      if (it.value()->machine_data_claim_tracker) {
        it.value()->machine_data_claim_tracker->OnTokenizationDone();
      }
    }
  }
  if (context->machine_data_claim_tracker) {
    context->machine_data_claim_tracker->OnTokenizationDone();
  }
}

MachineDataClaimTracker::TraceBounds*
MachineDataClaimTracker::GetOrCreateTraceBounds(
    TraceProcessorContext* context) {
  TraceId trace_id = context->trace_state ? context->trace_state->trace_id
                                          : context->trace_id();
  trace_contexts_[trace_id] = context;
  for (auto& tb : trace_bounds_) {
    if (tb.trace_id == trace_id) {
      last_noted_context_ = context;
      last_trace_bounds_ = &tb;
      return &tb;
    }
  }
  trace_bounds_.emplace_back();
  auto& tb = trace_bounds_.back();
  tb.trace_id = trace_id;
  last_noted_context_ = context;
  last_trace_bounds_ = &tb;
  return &tb;
}

const MachineDataClaimTracker::TraceBounds*
MachineDataClaimTracker::FindTraceBounds(TraceId trace_id) const {
  for (const auto& tb : trace_bounds_) {
    if (tb.trace_id == trace_id) {
      return &tb;
    }
  }
  return nullptr;
}

int64_t MachineDataClaimTracker::SchedLeaseEnd(TraceId trace_id) const {
  // If a decision happens before leases are final (e.g. streaming mode without
  // full sort), or if a trace has no recorded bounds, treat lease end as +inf
  // (Proposal B behavior).
  if (!tokenization_done_) {
    return std::numeric_limits<int64_t>::max();
  }
  // Never hand over from a trace whose sched slices cannot be closed.
  const auto* closer = sched_closers_.Find(trace_id);
  if (!closer || !*closer) {
    return std::numeric_limits<int64_t>::max();
  }
  const auto* tb = FindTraceBounds(trace_id);
  if (!tb) {
    return std::numeric_limits<int64_t>::max();
  }
  if (tb->sched_valid) {
    return tb->sched_max;
  }
  if (tb->overall_valid) {
    return tb->overall_max;
  }
  return std::numeric_limits<int64_t>::max();
}

int64_t MachineDataClaimTracker::CounterLeaseEnd(TraceId trace_id,
                                                 uint32_t source_mask) const {
  if (!tokenization_done_) {
    return std::numeric_limits<int64_t>::max();
  }
  const auto* tb = FindTraceBounds(trace_id);
  if (!tb) {
    return std::numeric_limits<int64_t>::max();
  }
  int64_t max_ts = std::numeric_limits<int64_t>::min();
  bool found = false;
  for (size_t k = 0; k < kSourceKindCount; ++k) {
    if ((source_mask & (1u << k)) && tb->kinds[k].valid) {
      max_ts = std::max(max_ts, tb->kinds[k].max);
      found = true;
    }
  }
  if (found) {
    return max_ts;
  }
  if (tb->overall_valid) {
    return tb->overall_max;
  }
  return std::numeric_limits<int64_t>::max();
}

void MachineDataClaimTracker::RegisterMachineTrack(TrackId track_id,
                                                   std::string_view type) {
  if (track_id.value >= track_claims_.size()) {
    track_claims_.resize(track_id.value + 1);
  }
  auto& claim = track_claims_[track_id.value];
  if (claim.is_machine_track) {
    return;
  }
  claim.is_machine_track = true;
  claim.type_id = context_->storage->InternString(type);

  uint32_t mask = 0;
  if (type == "cpu_frequency") {
    mask = kSourceFtrace | kSourceSysStats | kSourceGenericKernel |
           kSourceSystrace;
  } else if (type == "cpu_max_frequency_limit" ||
             type == "cpu_min_frequency_limit") {
    mask = kSourceFtrace | kSourceSystrace;
  } else if (type == "gpu_frequency") {
    mask = kSourceFtrace | kSourceSysStats | kSourceGenericKernel;
  } else if (type == "cpu_idle") {
    mask = kSourceFtrace | kSourceSystrace;
  } else if (type == "battery_counter") {
    mask = kSourceBattery | kSourceDumpstateBattery;
  } else if (type == "entity_state") {
    mask = kSourceEntityState;
  } else {
    PERFETTO_DCHECK(false);
  }
  claim.source_mask = mask;
}

void MachineDataClaimTracker::CloseOpenRowsForSched(TraceId old_owner,
                                                    int64_t close_ts) {
  auto* closer = sched_closers_.Find(old_owner);
  if (closer && *closer) {
    (*closer)(close_ts);
  }
  auto* o_ctx_ptr = trace_contexts_.Find(old_owner);
  if (o_ctx_ptr && *o_ctx_ptr && (*o_ctx_ptr)->thread_state_tracker) {
    ThreadStateTracker::GetOrCreate(*o_ctx_ptr)->CloseOpenStatesAt(close_ts);
  }
}

bool MachineDataClaimTracker::KeepCounterSlow(TraceProcessorContext* caller,
                                              TrackId track,
                                              int64_t ts,
                                              TraceId trace_id) {
  if (PERFETTO_UNLIKELY(!trace_contexts_.Find(trace_id))) {
    trace_contexts_[trace_id] = caller;
  }
  PERFETTO_DCHECK(track.value < track_claims_.size());
  auto& claim = track_claims_[track.value];
  if (!claim.is_machine_track) {
    return true;
  }

  if (!claim.owner.has_value()) {
    claim.owner = trace_id;
    claim.owner_lease_end = CounterLeaseEnd(trace_id, claim.source_mask);
    return true;
  }

  if (*claim.owner == trace_id) {
    return true;
  }

  if (ts > claim.owner_lease_end) {
    // Handover after owner lease end: incoming write becomes the new owner.
    claim.owner = trace_id;
    claim.owner_lease_end = CounterLeaseEnd(trace_id, claim.source_mask);
    return true;
  }

  // Drop inside owner's lease.
  auto dropped_key = std::make_pair(trace_id, claim.type_id);
  auto it = std::find(counter_dropped_types_.begin(),
                      counter_dropped_types_.end(), dropped_key);
  if (it != counter_dropped_types_.end()) {
    caller->stats_tracker->IncrementStats(
        stats::machine_counter_claim_conflict);
    return false;
  }
  counter_dropped_types_.push_back(dropped_key);
  if (caller->import_logs_tracker) {
    TraceId owner_id = *claim.owner;
    StringId type_id = claim.type_id;
    caller->import_logs_tracker->RecordParserLog(
        stats::machine_counter_claim_conflict, ts,
        [this, type_id, owner_id](ArgsTracker::BoundInserter& inserter) {
          inserter.AddArg(kind_key_id_, Variadic::String(counter_kind_id_));
          inserter.AddArg(counter_type_key_id_, Variadic::String(type_id));
          inserter.AddArg(
              owner_trace_id_key_id_,
              Variadic::Integer(static_cast<int64_t>(owner_id.value)));
        });
  } else {
    caller->stats_tracker->IncrementStats(
        stats::machine_counter_claim_conflict);
  }
  return false;
}

bool MachineDataClaimTracker::KeepSchedSlow(TraceProcessorContext* caller,
                                            SchedEventKind kind,
                                            int64_t ts,
                                            TraceId trace_id) {
  if (PERFETTO_UNLIKELY(!trace_contexts_.Find(trace_id))) {
    trace_contexts_[trace_id] = caller;
  }

  if (!sched_owner_.has_value()) {
    if (kind == SchedEventKind::kSwitch) {
      sched_owner_ = trace_id;
      for (auto it = trace_contexts_.GetIterator(); it; ++it) {
        if (it.key() != trace_id) {
          auto* ctx = it.value();
          if (ctx && ctx->thread_state_tracker) {
            ThreadStateTracker::GetOrCreate(ctx)->CloseOpenStatesAt(ts);
          }
        }
      }
      return true;
    }
    // Nobody owns scheduling yet: drop if an earlier-starting trace covers ts.
    if (auto covering = EarlierSchedTraceCovering(trace_id, ts); covering) {
      RecordSchedDrop(caller, trace_id, ts, *covering);
      return false;
    }
    return true;
  }

  if (*sched_owner_ == trace_id) {
    return true;
  }

  // Another trace owns sched.
  int64_t owner_lease_end = SchedLeaseEnd(*sched_owner_);
  if (kind == SchedEventKind::kSwitch && ts > owner_lease_end) {
    // Handover after owner lease end.
    CloseOpenRowsForSched(*sched_owner_, owner_lease_end);
    sched_owner_ = trace_id;
    return true;
  }

  // Drop event (inside owner lease, or kOther at any time while another trace
  // owns sched).
  RecordSchedDrop(caller, trace_id, ts, *sched_owner_);
  return false;
}

void MachineDataClaimTracker::RecordSchedDrop(TraceProcessorContext* caller,
                                              TraceId trace_id,
                                              int64_t ts,
                                              TraceId owner_id) {
  auto it = std::find(sched_dropped_traces_.begin(),
                      sched_dropped_traces_.end(), trace_id);
  if (it != sched_dropped_traces_.end() || !caller->import_logs_tracker) {
    caller->stats_tracker->IncrementStats(stats::machine_sched_claim_conflict);
    return;
  }
  sched_dropped_traces_.push_back(trace_id);
  caller->import_logs_tracker->RecordParserLog(
      stats::machine_sched_claim_conflict, ts,
      [this, owner_id](ArgsTracker::BoundInserter& inserter) {
        inserter.AddArg(kind_key_id_, Variadic::String(sched_kind_id_));
        inserter.AddArg(
            owner_trace_id_key_id_,
            Variadic::Integer(static_cast<int64_t>(owner_id.value)));
      });
}

std::optional<TraceId> MachineDataClaimTracker::EarlierSchedTraceCovering(
    TraceId trace_id,
    int64_t ts) const {
  // Traces are ordered by first sched event, then by trace id (archive order),
  // so that of two identical traces only the second one loses its events.
  const TraceBounds* mine = FindTraceBounds(trace_id);
  int64_t my_min = mine && mine->sched_valid
                       ? mine->sched_min
                       : std::numeric_limits<int64_t>::max();
  for (const auto& tb : trace_bounds_) {
    if (tb.trace_id == trace_id || !tb.sched_valid || ts < tb.sched_min ||
        ts > tb.sched_max) {
      continue;
    }
    if (tb.sched_min < my_min ||
        (tb.sched_min == my_min && tb.trace_id.value < trace_id.value)) {
      return tb.trace_id;
    }
  }
  return std::nullopt;
}

void MachineDataClaimTracker::RecordTaskNewTask(int64_t ts,
                                                uint32_t pid,
                                                UniqueTid utid) {
  if (ts > last_newtask_ts_) {
    applied_newtasks_.Clear();
    last_newtask_ts_ = ts;
  } else if (ts < last_newtask_ts_) {
    return;
  }
  applied_newtasks_.Insert(pid, utid);
}

std::optional<UniqueTid> MachineDataClaimTracker::FindTaskNewTask(
    int64_t ts,
    uint32_t pid) const {
  if (ts != last_newtask_ts_) {
    return std::nullopt;
  }
  const UniqueTid* utid = applied_newtasks_.Find(pid);
  return utid ? std::make_optional(*utid) : std::nullopt;
}

}  // namespace perfetto::trace_processor
