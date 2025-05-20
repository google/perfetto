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

#include "protos/perfetto/trace/generic_kernel/generic_task_state.pbzero.h"

namespace perfetto::trace_processor {

using protozero::ConstBytes;

using PendingSchedInfo = SchedEventState::PendingSchedInfo;

PERFETTO_ALWAYS_INLINE
void GenericKernelParser::InsertPendingStateInfoForTid(
    UniqueTid utid,
    SchedEventState::PendingSchedInfo sched_info) {
  if (utid >= pending_state_per_utid_.size()) {
    pending_state_per_utid_.resize(utid + 1);
  }
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
      running_string_id_(context_->storage->InternString("Running")),
      task_states_({
          context_->storage->InternString("Unknown"),
          context_->storage->InternString("Created"),
          context_->storage->InternString("R"),
          context_->storage->InternString("Running"),
          context_->storage->InternString("S"),
          context_->storage->InternString("D"),
          context_->storage->InternString("T"),
          context_->storage->InternString("Z"),
          context_->storage->InternString("X"),
      }) {}

void GenericKernelParser::ParseGenericTaskStateEvent(
    int64_t ts,
    protozero::ConstBytes data) {
  protos::pbzero::GenericKernelTaskStateEvent::Decoder task_event(data);

  StringId comm_id = context_->storage->InternString(task_event.comm());
  const uint32_t cpu = static_cast<uint32_t>(task_event.cpu());
  const uint32_t tid = static_cast<uint32_t>(task_event.tid());
  const int32_t prio = task_event.prio();

  // TODO(jahdiel): Handle the TASK_STATE_CREATED in order to set
  // the thread's creation timestamp.
  UniqueTid utid = context_->process_tracker->UpdateThreadName(
      tid, comm_id, ThreadNamePriority::kGenericKernelTask);

  StringId state_string_id =
      task_states_[static_cast<size_t>(task_event.state())];
  if (state_string_id == kNullStringId) {
    context_->storage->IncrementStats(stats::task_state_invalid);
  }

  // Given |PushSchedSwitch| updates the pending slice, run this
  // method before it.
  PendingSchedInfo prev_pending_sched =
      *sched_event_state_.GetPendingSchedInfoForCpu(cpu);

  // Handle context switches
  auto sched_switch_type =
      PushSchedSwitch(ts, cpu, tid, utid, state_string_id, prio);

  // Update the ThreadState table.
  switch (sched_switch_type) {
    case kUpdateEndState: {
      ThreadStateTracker::GetOrCreate(context_)->UpdatePendingState(
          utid, state_string_id);
      break;
    }
    case kStartWithPending: {
      ThreadStateTracker::GetOrCreate(context_)->PushThreadState(
          ts, prev_pending_sched.last_utid, kNullStringId);

      // Create the unknown thread state for the previous thread and
      // proceed to update the current thread's state.
      PERFETTO_FALLTHROUGH;
    }
    case kStart:
    case kClose:
    case kNone: {
      std::optional<uint16_t> cpu_op = state_string_id == running_string_id_
                                           ? std::optional{cpu}
                                           : std::nullopt;

      ThreadStateTracker::GetOrCreate(context_)->PushThreadState(
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
    auto rc = kStart;
    // Close the previous sched slice
    if (pending_slice_idx < std::numeric_limits<uint32_t>::max()) {
      context_->sched_event_tracker->ClosePendingSlice(pending_slice_idx, ts,
                                                       kNullStringId);
      InsertPendingStateInfoForTid(pending_sched->last_utid, *pending_sched);
      rc = kStartWithPending;
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
    return kClose;
  }
  // Add end state to a previously ended context switch if applicable.
  // For the end state to be added the timestamp of the event must match
  // the timestamp of the previous context switch.
  auto hanging_sched = GetPendingStateInfoForTid(utid);
  if (hanging_sched.has_value()) {
    auto sched_slice_idx = hanging_sched->pending_slice_storage_idx;
    auto close_ts =
        context_->sched_event_tracker->GetEndTimestampForPendingSlice(
            sched_slice_idx);
    if (ts == close_ts) {
      context_->sched_event_tracker->SetEndStateForPendingSlice(
          sched_slice_idx, state_string_id);
      RemovePendingStateInfoForTid(utid);
      return kUpdateEndState;
    }
  }
  return kNone;
}
}  // namespace perfetto::trace_processor
