/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_processor/importers/generic_kernel/generic_kernel_module.h"

#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/sched_event_tracker.h"
#include "src/trace_processor/importers/common/thread_state_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

#include "protos/perfetto/trace/generic_kernel/generic_event.pbzero.h"

namespace perfetto::trace_processor {

using protos::pbzero::TaskStateEnum;
using protozero::ConstBytes;

using PendingSchedInfo = SchedEventState::PendingSchedInfo;

constexpr std::array<const char*, 8> kTaskStates = {
    "Created", "R", "Running", "S", "D", "T", "Z", "X"};

PERFETTO_ALWAYS_INLINE
StringId GenericKernelParser::TaskStateToStringId(size_t state) {
  return state < kTaskStates.size()
             ? context_->storage->InternString(kTaskStates[state])
             : kNullStringId;
}

PERFETTO_ALWAYS_INLINE
void GenericKernelParser::InsertPendingStateInfoForTid(
    UniqueTid utid,
    SchedEventState::PendingSchedInfo sched_info) {
  if (utid >= pending_state_per_utid_.size()) {
    pending_state_per_utid_.resize(utid + 1);
  }
  // Overrides any old hanging slice for the utid
  pending_state_per_utid_[utid] = sched_info;
}

PERFETTO_ALWAYS_INLINE
std::optional<SchedEventState::PendingSchedInfo>
GenericKernelParser::GetPendingStateInfoForTid(UniqueTid utid) {
  return utid < pending_state_per_utid_.size() ? pending_state_per_utid_[utid]
                                               : std::nullopt;
}

PERFETTO_ALWAYS_INLINE
void GenericKernelParser::RemovePendingStateInfoForTid(UniqueTid utid) {
  if (utid < pending_state_per_utid_.size()) {
    pending_state_per_utid_[utid].reset();
  }
}

GenericKernelParser::GenericKernelParser(TraceProcessorContext* context)
    : context_(context),
      running_string_id_(context_->storage->InternString("Running")) {}

void GenericKernelParser::ParseGenericTaskStateEvent(
    int64_t ts,
    protozero::ConstBytes data) {
  protos::pbzero::GenericTaskStateEvent::Decoder task_event(data);

  StringId comm_id = context_->storage->InternString(task_event.comm());
  const uint32_t cpu = static_cast<uint32_t>(task_event.cpu());
  const uint32_t tid = static_cast<uint32_t>(task_event.tid());
  const int32_t prio = task_event.prio();

  // TODO(jahdiel): Handle the TASK_STATE_CREATED in order to set
  // the thread's creation timestamp.
  UniqueTid utid = context_->process_tracker->UpdateThreadName(
      tid, comm_id, ThreadNamePriority::kGenericKernelTask);

  StringId state_string_id =
      TaskStateToStringId(static_cast<size_t>(task_event.state()));
  if (state_string_id == kNullStringId) {
    context_->storage->IncrementStats(stats::task_state_invalid);
  }

  // Given |PushSchedSwitch| updates the pending slice, run this
  // method before it.
  PendingSchedInfo prev_pending_sched =
      *sched_event_state_.GetPendingSchedInfoForCpu(cpu);

  // Handle context switches
  auto schedSwitchType =
      PushSchedSwitch(ts, cpu, tid, utid, state_string_id, prio);

  // Update the ThreadState table.
  switch (schedSwitchType) {
    case SCHED_SWITCH_UPDATE_END_STATE: {
      ThreadStateTracker::GetOrCreate(context_)->UpdateOpenState(
          utid, state_string_id);
      break;
    }
    case SCHED_SWITCH_START_WITH_PENDING: {
      ThreadStateTracker::GetOrCreate(context_)->ClosePendingState(
          ts, prev_pending_sched.last_utid, false /*data_loss*/);

      ThreadStateTracker::GetOrCreate(context_)->AddOpenState(
          ts, prev_pending_sched.last_utid, kNullStringId);

      // Create the unknown thread state for the previous thread and
      // proceed to update the current thread's state.
      [[fallthrough]];
    }
    case SCHED_SWITCH_START:
    case SCHED_SWITCH_CLOSE:
    case SCHED_SWITCH_NONE: {
      ThreadStateTracker::GetOrCreate(context_)->ClosePendingState(
          ts, utid, false /*data_loss*/);

      std::optional<uint16_t> cpu_op = state_string_id == running_string_id_
                                           ? std::optional{cpu}
                                           : std::nullopt;

      ThreadStateTracker::GetOrCreate(context_)->AddOpenState(
          ts, utid, state_string_id, cpu_op);
      break;
    }
  }
}

// Handles context switches based on GenericTaskStateEvents.
//
// Given the task state events only capture the state of a single
// task, parsing context switches becomes asynchronous because,
// the start and end events could be received in different orders.
// To manage this we need to consider both of these scenarios
// for each CPU:
//
//   start task1 -> close task1 -> start task2
//   start task1 -> start task2 -> close task1
//
// The first scenario is straightforward. For the second scenario
// we keep track of any hanging opened slices. When the closing
// event is received, we then proceed add the end_state to the
// sched_slice table.
GenericKernelParser::SchedSwitchType GenericKernelParser::PushSchedSwitch(
    int64_t ts,
    uint32_t cpu,
    uint32_t tid,
    UniqueTid utid,
    StringId state_string_id,
    int32_t prio) {
  auto* pending_sched = sched_event_state_.GetPendingSchedInfoForCpu(cpu);
  uint32_t pending_slice_idx = pending_sched->pending_slice_storage_idx;
  if (state_string_id == running_string_id_) {
    auto rc = SCHED_SWITCH_START;
    // Close the previous sched slice
    if (pending_slice_idx < std::numeric_limits<uint32_t>::max()) {
      context_->sched_event_tracker->ClosePendingSlice(pending_slice_idx, ts,
                                                       kNullStringId);
      InsertPendingStateInfoForTid(pending_sched->last_utid, *pending_sched);
      rc = SCHED_SWITCH_START_WITH_PENDING;
    }
    // Start a new sched slice for the new task.
    auto new_slice_idx =
        context_->sched_event_tracker->AddStartSlice(cpu, ts, utid, prio);

    pending_sched->pending_slice_storage_idx = new_slice_idx;
    pending_sched->last_pid = tid;
    pending_sched->last_utid = utid;
    pending_sched->last_prio = prio;
    return rc;
  }
  // Close the pending slice if applicable
  if (pending_slice_idx < std::numeric_limits<uint32_t>::max() &&
      tid == pending_sched->last_pid) {
    context_->sched_event_tracker->ClosePendingSlice(pending_slice_idx, ts,
                                                     state_string_id);
    // Clear the pending slice
    *pending_sched = SchedEventState::PendingSchedInfo();
    return SCHED_SWITCH_CLOSE;
  }
  // Add end state to a previously ended context switch if applicable.
  // For the end state to be added the timestamp of the event must match
  // the timestamp of the previous context switch.
  auto hanging_sched = GetPendingStateInfoForTid(utid);
  if (hanging_sched.has_value()) {
    auto sched_slice_idx = hanging_sched->pending_slice_storage_idx;
    auto close_ts =
        context_->sched_event_tracker->GetCloseTimestamp(sched_slice_idx);
    if (ts == close_ts) {
      context_->sched_event_tracker->SetEndStateToSlice(sched_slice_idx,
                                                        state_string_id);
      RemovePendingStateInfoForTid(utid);
      return SCHED_SWITCH_UPDATE_END_STATE;
    }
  }
  return SCHED_SWITCH_NONE;
}
}  // namespace perfetto::trace_processor
