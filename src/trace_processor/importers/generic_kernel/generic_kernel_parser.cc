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

#include "protos/perfetto/trace/generic/generic_event.pbzero.h"

namespace perfetto::trace_processor {

using protos::pbzero::TaskStateEnum;
using protozero::ConstBytes;

GenericKernelParser::GenericKernelParser(TraceProcessorContext* context)
    : context_(context),
      running_string_id_(context_->storage->InternString("Running")) {}

void GenericKernelParser::ParseGenericTaskStateEvent(
    int64_t ts,
    protozero::ConstBytes data) {
  protos::pbzero::GenericTaskStateEvent::Decoder task_event(data);

  StringId comm_id = context_->storage->InternString(task_event.comm());
  const int32_t cpu = task_event.cpu();
  const uint32_t tid = static_cast<uint32_t>(task_event.tid());
  const int32_t prio = task_event.prio();

  UniqueTid utid = context_->process_tracker->UpdateThreadName(
      tid, comm_id, ThreadNamePriority::kGenericKernelTask);

  StringId state_string_id = TaskStateToStringId(task_event.state());
  if (state_string_id == kNullStringId) {
    context_->storage->IncrementStats(stats::task_state_invalid);
  }

  // Handle context switches
  PushSchedSwitch(ts, cpu, tid, utid, state_string_id, prio);

  // Update the ThreadState table.
  ThreadStateTracker::GetOrCreate(context_)->ClosePendingState(
      ts, utid, false /*data_loss*/);

  std::optional<uint16_t> cpu_op =
      state_string_id == running_string_id_ ? std::optional{cpu} : std::nullopt;

  ThreadStateTracker::GetOrCreate(context_)->AddOpenState(
      ts, utid, state_string_id, cpu_op);
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
void GenericKernelParser::PushSchedSwitch(int64_t ts,
                                          int32_t cpu,
                                          uint32_t tid,
                                          UniqueTid utid,
                                          StringId state_string_id,
                                          int32_t prio) {
  auto* pending_sched = sched_event_state_.GetPendingSchedInfoForCpu(cpu);
  uint32_t pending_slice_idx = pending_sched->pending_slice_storage_idx;
  if (state_string_id == running_string_id_) {
    // Close the previous sched slice
    if (pending_slice_idx < std::numeric_limits<uint32_t>::max()) {
      context_->sched_event_tracker->ClosePendingSlice(pending_slice_idx, ts,
                                                       kNullStringId);
      sched_event_state_.InsertHangingSchedInfoForTid(pending_sched->last_utid,
                                                      *pending_sched);
    }
    // Start a new sched slice for the new task.
    auto new_slice_idx =
        context_->sched_event_tracker->AddStartSlice(cpu, ts, utid, prio);

    pending_sched->pending_slice_storage_idx = new_slice_idx;
    pending_sched->last_pid = tid;
    pending_sched->last_utid = utid;
    pending_sched->last_prio = prio;
  } else {
    // Close the pending slice if applicable
    if (pending_slice_idx < std::numeric_limits<uint32_t>::max() &&
        tid == pending_sched->last_pid) {
      context_->sched_event_tracker->ClosePendingSlice(pending_slice_idx, ts,
                                                       state_string_id);
      // Clear the pending slice
      *pending_sched = SchedEventState::PendingSchedInfo();
    } else {
      // Close any hanging slice associated with the utid
      auto* hanging_sched = sched_event_state_.GetHangingSchedInfoForTid(utid);
      if (hanging_sched) {
        context_->sched_event_tracker->SetEndStateToSlice(
            hanging_sched->pending_slice_storage_idx, state_string_id);
        sched_event_state_.EraseHangingSchedInfoForTid(utid);
      }
    }
  }
}

StringId GenericKernelParser::TaskStateToStringId(
    [[maybe_unused]] int32_t state) {
  std::map<uint32_t, base::StringView> task_states_map = {
      {TaskStateEnum::TASK_STATE_CREATED, "Created"},
      {TaskStateEnum::TASK_STATE_RUNNABLE, "R"},
      {TaskStateEnum::TASK_STATE_RUNNING, "Running"},
      {TaskStateEnum::TASK_STATE_INTERRUPTIBLE_SLEEP, "S"},
      {TaskStateEnum::TASK_STATE_UNINTERRUPTIBLE_SLEEP, "D"},
      {TaskStateEnum::TASK_STATE_STOPPED, "T"},
      {TaskStateEnum::TASK_STATE_DEAD, "Z"},
      {TaskStateEnum::TASK_STATE_DESTROYED, "X"},
  };
  return task_states_map.find(state) != task_states_map.end()
             ? context_->storage->InternString(task_states_map[state])
             : kNullStringId;
}

}  // namespace perfetto::trace_processor
