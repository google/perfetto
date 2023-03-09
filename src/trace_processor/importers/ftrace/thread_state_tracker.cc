/*
 * Copyright (C) 2020 The Android Open Source Project
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
#include "src/trace_processor/importers/ftrace/thread_state_tracker.h"

namespace perfetto {
namespace trace_processor {
ThreadStateTracker::ThreadStateTracker(TraceStorage* storage)
    : storage_(storage),
      running_string_id_(storage->InternString("Running")),
      runnable_string_id_(storage->InternString("R")) {}
ThreadStateTracker::~ThreadStateTracker() = default;

void ThreadStateTracker::PushSchedSwitchEvent(int64_t event_ts,
                                              uint32_t cpu,
                                              UniqueTid prev_utid,
                                              StringId prev_state,
                                              UniqueTid next_utid) {
  // Code related to previous utid. If the thread wasn't running before we know
  // we lost data and should close the slice accordingly.
  bool data_loss_cond =
      HasPreviousRowNumbersForUtid(prev_utid) &&
      !IsRunning(RowNumToRef(prev_row_numbers_for_thread_[prev_utid]->last_row)
                     .state());
  ClosePendingState(event_ts, prev_utid, data_loss_cond);
  AddOpenState(event_ts, prev_utid, prev_state);

  // Code related to next utid.
  // Due to forced migration, it is possible for the same thread to be
  // scheduled on different CPUs at the same time.
  // We work around this problem by truncating the previous state to the start
  // of this state and starting the next state normally. This is why we don't
  // check whether previous state is running/runnable. See b/186509316 for
  // details and an example on when this happens.
  ClosePendingState(event_ts, next_utid, false);
  AddOpenState(event_ts, next_utid, running_string_id_, cpu);
}

void ThreadStateTracker::PushWakingEvent(int64_t event_ts,
                                         UniqueTid utid,
                                         UniqueTid waker_utid) {
  // Only open new runnable state if thread already had a sched switch event.
  if (!HasPreviousRowNumbersForUtid(utid)) {
    return;
  }
  auto last_row_ref = RowNumToRef(prev_row_numbers_for_thread_[utid]->last_row);

  // Occasionally, it is possible to get a waking event for a thread
  // which is already in a runnable state. When this happens (or if the thread
  // is running), we just ignore the waking event. See b/186509316 for details
  // and an example on when this happens. Only blocked events can be waken up.
  if (!IsBlocked(last_row_ref.state())) {
    return;
  }

  // Close the sleeping state and open runnable state.
  ClosePendingState(event_ts, utid, false);
  AddOpenState(event_ts, utid, runnable_string_id_, base::nullopt, waker_utid);
}

void ThreadStateTracker::PushNewTaskEvent(int64_t event_ts,
                                         UniqueTid utid,
                                         UniqueTid waker_utid) {
  AddOpenState(event_ts, utid, runnable_string_id_, base::nullopt, waker_utid);
}

void ThreadStateTracker::PushBlockedReason(
    UniqueTid utid,
    base::Optional<bool> io_wait,
    base::Optional<StringId> blocked_function) {
  // Return if there is no state, as there is are no previous rows available.
  if (!HasPreviousRowNumbersForUtid(utid))
    return;

  // Return if no previous bocked row exists.
  auto blocked_row_number =
      prev_row_numbers_for_thread_[utid]->last_blocked_row;
  if (!blocked_row_number.has_value())
    return;

  auto row_reference = RowNumToRef(blocked_row_number.value());
  if (io_wait.has_value()) {
    row_reference.set_io_wait(*io_wait);
  }
  if (blocked_function.has_value()) {
    row_reference.set_blocked_function(*blocked_function);
  }
}

void ThreadStateTracker::AddOpenState(int64_t ts,
                                      UniqueTid utid,
                                      StringId state,
                                      base::Optional<uint32_t> cpu,
                                      base::Optional<UniqueTid> waker_utid) {
  // Ignore utid 0 because it corresponds to the swapper thread which doesn't
  // make sense to insert.
  if (utid == 0)
    return;

  // Insert row with unfinished state
  tables::ThreadStateTable::Row row;
  row.ts = ts;
  row.cpu = cpu;
  row.waker_utid = waker_utid;
  row.dur = -1;
  row.utid = utid;
  row.state = state;
  auto row_num = storage_->mutable_thread_state_table()->Insert(row).row_number;

  if (utid >= prev_row_numbers_for_thread_.size()) {
    prev_row_numbers_for_thread_.resize(utid + 1);
  }

  if (!prev_row_numbers_for_thread_[utid].has_value()) {
    prev_row_numbers_for_thread_[utid] = RelatedRows{base::nullopt, row_num};
  }

  if (IsRunning(state)) {
    prev_row_numbers_for_thread_[utid] = RelatedRows{base::nullopt, row_num};
  } else if (IsBlocked(state)) {
    prev_row_numbers_for_thread_[utid] = RelatedRows{row_num, row_num};
  } else /* if (IsRunnable(state)) */ {
    prev_row_numbers_for_thread_[utid]->last_row = row_num;
  }
}

void ThreadStateTracker::ClosePendingState(int64_t end_ts,
                                           UniqueTid utid,
                                           bool data_loss) {
  // Discard close if there is no open state to close.
  if (!HasPreviousRowNumbersForUtid(utid))
    return;

  auto row_ref = RowNumToRef(prev_row_numbers_for_thread_[utid]->last_row);

  // Update the duration only for states without data loss.
  if (!data_loss) {
    row_ref.set_dur(end_ts - row_ref.ts());
  }
}

bool ThreadStateTracker::IsRunning(StringId state) {
  return state == running_string_id_;
}

bool ThreadStateTracker::IsRunnable(StringId state) {
  return state == runnable_string_id_;
}

bool ThreadStateTracker::IsBlocked(StringId state) {
  return !(IsRunnable(state) || IsRunning(state));
}

}  // namespace trace_processor
}  // namespace perfetto
