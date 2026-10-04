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
#include <optional>
#include <string_view>

#include "perfetto/base/logging.h"
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
  if (!claim.owner.has_value()) {
    claim.owner = trace_id;
    return true;
  }
  if (*claim.owner == trace_id) {
    return true;
  }

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
    }
    return true;
  }
  if (*sched_owner_ == trace_id) {
    return true;
  }

  auto it = std::find(sched_dropped_traces_.begin(),
                      sched_dropped_traces_.end(), trace_id);
  if (it != sched_dropped_traces_.end()) {
    caller->stats_tracker->IncrementStats(stats::machine_sched_claim_conflict);
    return false;
  }
  sched_dropped_traces_.push_back(trace_id);
  if (caller->import_logs_tracker) {
    TraceId owner_id = *sched_owner_;
    caller->import_logs_tracker->RecordParserLog(
        stats::machine_sched_claim_conflict, ts,
        [this, owner_id](ArgsTracker::BoundInserter& inserter) {
          inserter.AddArg(kind_key_id_, Variadic::String(sched_kind_id_));
          inserter.AddArg(
              owner_trace_id_key_id_,
              Variadic::Integer(static_cast<int64_t>(owner_id.value)));
        });
  } else {
    caller->stats_tracker->IncrementStats(stats::machine_sched_claim_conflict);
  }
  return false;
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
