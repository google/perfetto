/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/importers/ftrace/sched_event_tracker.h"

#include <math.h>

#include "perfetto/ext/base/utils.h"
#include "src/trace_processor/args_tracker.h"
#include "src/trace_processor/event_tracker.h"
#include "src/trace_processor/ftrace_utils.h"
#include "src/trace_processor/importers/ftrace/ftrace_descriptors.h"
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/stats.h"
#include "src/trace_processor/trace_processor_context.h"
#include "src/trace_processor/variadic.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/sched.pbzero.h"

namespace perfetto {
namespace trace_processor {

SchedEventTracker::SchedEventTracker(TraceProcessorContext* context)
    : context_(context) {
  // pre-parse sched_switch
  auto* switch_descriptor = GetMessageDescriptorForId(
      protos::pbzero::FtraceEvent::kSchedSwitchFieldNumber);
  PERFETTO_CHECK(switch_descriptor->max_field_id == kSchedSwitchMaxFieldId);

  for (size_t i = 1; i <= kSchedSwitchMaxFieldId; i++) {
    sched_switch_field_ids_[i] =
        context->storage->InternString(switch_descriptor->fields[i].name);
  }
  sched_switch_id_ = context->storage->InternString(switch_descriptor->name);

  // pre-parse sched_waking
  auto* waking_descriptor = GetMessageDescriptorForId(
      protos::pbzero::FtraceEvent::kSchedWakingFieldNumber);
  PERFETTO_CHECK(waking_descriptor->max_field_id == kSchedWakingMaxFieldId);

  for (size_t i = 1; i <= kSchedWakingMaxFieldId; i++) {
    sched_waking_field_ids_[i] =
        context->storage->InternString(waking_descriptor->fields[i].name);
  }
  sched_waking_id_ = context->storage->InternString(waking_descriptor->name);
}

SchedEventTracker::~SchedEventTracker() = default;

void SchedEventTracker::PushSchedSwitch(uint32_t cpu,
                                        int64_t ts,
                                        uint32_t prev_pid,
                                        base::StringView prev_comm,
                                        int32_t prev_prio,
                                        int64_t prev_state,
                                        uint32_t next_pid,
                                        base::StringView next_comm,
                                        int32_t next_prio) {
  // At this stage all events should be globally timestamp ordered.
  if (ts < context_->event_tracker->max_timestamp()) {
    PERFETTO_ELOG("sched_switch event out of order by %.4f ms, skipping",
                  (context_->event_tracker->max_timestamp() - ts) / 1e6);
    context_->storage->IncrementStats(stats::sched_switch_out_of_order);
    return;
  }
  context_->event_tracker->UpdateMaxTimestamp(ts);
  PERFETTO_DCHECK(cpu < base::kMaxCpus);

  StringId next_comm_id = context_->storage->InternString(next_comm);
  auto next_utid =
      context_->process_tracker->UpdateThreadName(next_pid, next_comm_id);

  // First use this data to close the previous slice.
  bool prev_pid_match_prev_next_pid = false;
  auto* pending_sched = &pending_sched_per_cpu_[cpu];
  size_t pending_slice_idx = pending_sched->pending_slice_storage_idx;
  if (pending_slice_idx < std::numeric_limits<size_t>::max()) {
    prev_pid_match_prev_next_pid = prev_pid == pending_sched->last_pid;
    if (PERFETTO_LIKELY(prev_pid_match_prev_next_pid)) {
      ClosePendingSlice(pending_slice_idx, ts, prev_state);
    } else {
      // If the pids are not consistent, make a note of this.
      context_->storage->IncrementStats(stats::mismatched_sched_switch_tids);
    }
  }

  // We have to intern prev_comm again because our assumption that
  // this event's |prev_comm| == previous event's |next_comm| does not hold
  // if the thread changed its name while scheduled.
  StringId prev_comm_id = context_->storage->InternString(prev_comm);
  UniqueTid prev_utid =
      context_->process_tracker->UpdateThreadName(prev_pid, prev_comm_id);

  auto new_slice_idx = AddRawEventAndStartSlice(
      cpu, ts, prev_utid, prev_pid, prev_comm_id, prev_prio, prev_state,
      next_utid, next_pid, next_comm_id, next_prio);

  // Finally, update the info for the next sched switch on this CPU.
  pending_sched->pending_slice_storage_idx = new_slice_idx;
  pending_sched->last_pid = next_pid;
  pending_sched->last_utid = next_utid;
  pending_sched->last_prio = next_prio;
}

void SchedEventTracker::PushSchedSwitchCompact(uint32_t cpu,
                                               int64_t ts,
                                               int64_t prev_state,
                                               uint32_t next_pid,
                                               int32_t next_prio,
                                               StringId next_comm_id) {
  // At this stage all events should be globally timestamp ordered.
  if (ts < context_->event_tracker->max_timestamp()) {
    PERFETTO_ELOG("sched_switch event out of order by %.4f ms, skipping",
                  (context_->event_tracker->max_timestamp() - ts) / 1e6);
    context_->storage->IncrementStats(stats::sched_switch_out_of_order);
    return;
  }
  context_->event_tracker->UpdateMaxTimestamp(ts);
  PERFETTO_DCHECK(cpu < base::kMaxCpus);

  auto next_utid =
      context_->process_tracker->UpdateThreadName(next_pid, next_comm_id);

  auto* pending_sched = &pending_sched_per_cpu_[cpu];

  // If we're processing the first compact event for this cpu, don't start a
  // slice since we're missing the "prev_*" fields. The successive events will
  // create slices as normal, but the first per-cpu switch is effectively
  // discarded.
  if (pending_sched->last_utid == std::numeric_limits<UniqueTid>::max()) {
    context_->storage->IncrementStats(stats::compact_sched_switch_skipped);

    pending_sched->last_pid = next_pid;
    pending_sched->last_utid = next_utid;
    pending_sched->last_prio = next_prio;
    // Note: no pending slice, so leave |pending_slice_storage_idx| in its
    // invalid state.
    return;
  }

  // Close the pending slice if any (we won't have one when processing the first
  // two compact events for a given cpu).
  size_t pending_slice_idx = pending_sched->pending_slice_storage_idx;
  if (pending_slice_idx < std::numeric_limits<size_t>::max())
    ClosePendingSlice(pending_slice_idx, ts, prev_state);

  // Use the previous event's values to infer this event's "prev_*" fields.
  // There are edge cases, but this assumption should still produce sensible
  // results in the absence of data loss.
  UniqueTid prev_utid = pending_sched->last_utid;
  uint32_t prev_pid = pending_sched->last_pid;
  int32_t prev_prio = pending_sched->last_prio;

  // Do a fresh task name lookup in case it was updated by a task_rename while
  // scheduled.
  const auto& prev_thread = context_->storage->GetThread(prev_utid);
  StringId prev_comm_id = prev_thread.name_id;

  auto new_slice_idx = AddRawEventAndStartSlice(
      cpu, ts, prev_utid, prev_pid, prev_comm_id, prev_prio, prev_state,
      next_utid, next_pid, next_comm_id, next_prio);

  // Finally, update the info for the next sched switch on this CPU.
  pending_sched->pending_slice_storage_idx = new_slice_idx;
  pending_sched->last_pid = next_pid;
  pending_sched->last_utid = next_utid;
  pending_sched->last_prio = next_prio;
}

PERFETTO_ALWAYS_INLINE
size_t SchedEventTracker::AddRawEventAndStartSlice(uint32_t cpu,
                                                   int64_t ts,
                                                   UniqueTid prev_utid,
                                                   uint32_t prev_pid,
                                                   StringId prev_comm_id,
                                                   int32_t prev_prio,
                                                   int64_t prev_state,
                                                   UniqueTid next_utid,
                                                   uint32_t next_pid,
                                                   StringId next_comm_id,
                                                   int32_t next_prio) {
  // Push the raw event - this is done as the raw ftrace event codepath does
  // not insert sched_switch.
  auto rid = context_->storage->mutable_raw_events()->AddRawEvent(
      ts, sched_switch_id_, cpu, prev_utid);

  // Note: this ordering is important. The events should be pushed in the same
  // order as the order of fields in the proto; this is used by the raw table to
  // index these events using the field ids.
  using SS = protos::pbzero::SchedSwitchFtraceEvent;
  auto add_raw_arg = [this](RowId row_id, int field_num, Variadic var) {
    StringId key = sched_switch_field_ids_[static_cast<size_t>(field_num)];
    context_->args_tracker->AddArg(row_id, key, key, var);
  };
  add_raw_arg(rid, SS::kPrevCommFieldNumber, Variadic::String(prev_comm_id));
  add_raw_arg(rid, SS::kPrevPidFieldNumber, Variadic::Integer(prev_pid));
  add_raw_arg(rid, SS::kPrevPrioFieldNumber, Variadic::Integer(prev_prio));
  add_raw_arg(rid, SS::kPrevStateFieldNumber, Variadic::Integer(prev_state));
  add_raw_arg(rid, SS::kNextCommFieldNumber, Variadic::String(next_comm_id));
  add_raw_arg(rid, SS::kNextPidFieldNumber, Variadic::Integer(next_pid));
  add_raw_arg(rid, SS::kNextPrioFieldNumber, Variadic::Integer(next_prio));

  // Open a new scheduling slice, corresponding to the task that was
  // just switched to.
  return context_->storage->mutable_slices()->AddSlice(
      cpu, ts, 0 /* duration */, next_utid, ftrace_utils::TaskState(),
      next_prio);
}

PERFETTO_ALWAYS_INLINE
void SchedEventTracker::ClosePendingSlice(size_t pending_slice_idx,
                                          int64_t ts,
                                          int64_t prev_state) {
  auto* slices = context_->storage->mutable_slices();

  int64_t duration = ts - slices->start_ns()[pending_slice_idx];
  slices->set_duration(pending_slice_idx, duration);

  // We store the state as a uint16 as we only consider values up to 2048
  // when unpacking the information inside; this allows savings of 48 bits
  // per slice.
  auto task_state = ftrace_utils::TaskState(static_cast<uint16_t>(prev_state));
  if (!task_state.is_valid()) {
    context_->storage->IncrementStats(stats::task_state_invalid);
  }
  slices->set_end_state(pending_slice_idx, task_state);
}

// Processes a sched_waking that was decoded from a compact representation,
// adding to the raw and instants tables.
void SchedEventTracker::PushSchedWakingCompact(uint32_t cpu,
                                               int64_t ts,
                                               uint32_t wakee_pid,
                                               int32_t target_cpu,
                                               int32_t prio,
                                               StringId comm_id) {
  // At this stage all events should be globally timestamp ordered.
  if (ts < context_->event_tracker->max_timestamp()) {
    PERFETTO_ELOG("sched_waking event out of order by %.4f ms, skipping",
                  (context_->event_tracker->max_timestamp() - ts) / 1e6);
    context_->storage->IncrementStats(stats::sched_waking_out_of_order);
    return;
  }
  context_->event_tracker->UpdateMaxTimestamp(ts);
  PERFETTO_DCHECK(cpu < base::kMaxCpus);

  // We infer the task that emitted the event (i.e. common_pid) from the
  // scheduling slices. Drop the event if we haven't seen any sched_switch
  // events for this cpu yet.
  // Note that if sched_switch wasn't enabled, we will have to skip all
  // compact waking events.
  auto* pending_sched = &pending_sched_per_cpu_[cpu];
  if (pending_sched->last_utid == std::numeric_limits<UniqueTid>::max()) {
    context_->storage->IncrementStats(stats::compact_sched_waking_skipped);
    return;
  }
  auto curr_utid = pending_sched->last_utid;

  // Add an entry to the raw table.
  auto rid = context_->storage->mutable_raw_events()->AddRawEvent(
      ts, sched_waking_id_, cpu, curr_utid);

  // "success" is hardcoded as always 1 by the kernel, with a TODO to remove it.
  static constexpr int32_t kHardcodedSuccess = 1;

  using SW = protos::pbzero::SchedWakingFtraceEvent;
  auto add_raw_arg = [this](RowId row_id, int field_num, Variadic var) {
    StringId key = sched_waking_field_ids_[static_cast<size_t>(field_num)];
    context_->args_tracker->AddArg(row_id, key, key, var);
  };
  add_raw_arg(rid, SW::kCommFieldNumber, Variadic::String(comm_id));
  add_raw_arg(rid, SW::kPidFieldNumber, Variadic::Integer(wakee_pid));
  add_raw_arg(rid, SW::kPrioFieldNumber, Variadic::Integer(prio));
  add_raw_arg(rid, SW::kSuccessFieldNumber,
              Variadic::Integer(kHardcodedSuccess));
  add_raw_arg(rid, SW::kTargetCpuFieldNumber, Variadic::Integer(target_cpu));

  // Add a waking entry to the instants.
  auto wakee_utid = context_->process_tracker->GetOrCreateThread(wakee_pid);
  auto* instants = context_->storage->mutable_instants();
  instants->AddInstantEvent(ts, sched_waking_id_, /*value=*/0, wakee_utid,
                            RefType::kRefUtid);
}

void SchedEventTracker::FlushPendingEvents() {
  // TODO(lalitm): the day this method is called before end of trace, don't
  // flush the sched events as they will probably be pushed in the next round
  // of ftrace events.
  int64_t end_ts = context_->storage->GetTraceTimestampBoundsNs().second;
  auto* slices = context_->storage->mutable_slices();
  for (const auto& pending_sched : pending_sched_per_cpu_) {
    size_t row = pending_sched.pending_slice_storage_idx;
    if (row == std::numeric_limits<size_t>::max())
      continue;

    int64_t duration = end_ts - slices->start_ns()[row];
    slices->set_duration(row, duration);
    slices->set_end_state(
        row, ftrace_utils::TaskState(ftrace_utils::TaskState::kRunnable));
  }

  pending_sched_per_cpu_ = {};
}

}  // namespace trace_processor
}  // namespace perfetto
