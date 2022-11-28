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

#ifndef SRC_TRACE_PROCESSOR_TABLES_SLICE_TABLES_H_
#define SRC_TRACE_PROCESSOR_TABLES_SLICE_TABLES_H_

#include "src/trace_processor/tables/macros.h"
#include "src/trace_processor/tables/track_tables.h"

namespace perfetto {
namespace trace_processor {
namespace tables {

// @name slice
// @tablegroup Events
// @param ts timestamp of the start of the slice (in nanoseconds)
// @param dur duration of the slice (in nanoseconds)
// @param arg_set_id {@joinable args.arg_set_id}
// @param thread_instruction_count The value of the CPU instruction counter at
// the start of the slice.
// @param thread_instruction_delta The change in value from
// @param thread_instruction_count to the end of the slice.
#define PERFETTO_TP_SLICE_TABLE_DEF(NAME, PARENT, C)   \
  NAME(SliceTable, "internal_slice")                   \
  PERFETTO_TP_ROOT_TABLE(PARENT, C)                    \
  C(int64_t, ts, Column::Flag::kSorted)                \
  C(int64_t, dur)                                      \
  C(TrackTable::Id, track_id)                          \
  C(base::Optional<StringPool::Id>, category)          \
  C(base::Optional<StringPool::Id>, name)              \
  C(uint32_t, depth)                                   \
  C(int64_t, stack_id)                                 \
  C(int64_t, parent_stack_id)                          \
  C(base::Optional<SliceTable::Id>, parent_id)         \
  C(uint32_t, arg_set_id)                              \
  C(base::Optional<int64_t>, thread_ts)                \
  C(base::Optional<int64_t>, thread_dur)               \
  C(base::Optional<int64_t>, thread_instruction_count) \
  C(base::Optional<int64_t>, thread_instruction_delta)

PERFETTO_TP_TABLE(PERFETTO_TP_SLICE_TABLE_DEF);

// @name sched_slice
//   This table holds slices with kernel thread scheduling information.
//   These slices are collected when the Linux "ftrace" data source is
//   used with the "sched/switch" and "sched/wakeup*" events enabled.
// @tablegroup Events
// @param id The row id for the table row.
// @param type This field always contains the string 'sched_slice'.
// @param ts The timestamp at the start of the slice (in nanoseconds).
// @param dur The duration of the slice (in nanoseconds).
// @param utid The thread's unique id in the trace. {@joinable thread.utid}.
// @param cpu The CPU that the slice executed on.
// @param end_state A string representing the scheduling state of the
//   kernel thread at the end of the slice.  The individual characters in
//   the string mean the following: R (runnable), S (awaiting a wakeup),
//   D (in an uninterruptible sleep), T (suspended), t (being traced),
//   X (exiting), P (parked), W (waking), I (idle), N (not contributing
//   to the load average), K (wakeable on fatal signals) and
//   Z (zombie, awaiting cleanup).
// @param priority The kernel priority that the thread ran at.
#define PERFETTO_TP_SCHED_SLICE_TABLE_DEF(NAME, PARENT, C) \
  NAME(SchedSliceTable, "sched_slice")                     \
  PERFETTO_TP_ROOT_TABLE(PARENT, C)                        \
  C(int64_t, ts, Column::Flag::kSorted)                    \
  C(int64_t, dur)                                          \
  C(uint32_t, cpu)                                         \
  C(uint32_t, utid)                                        \
  C(StringPool::Id, end_state)                             \
  C(int32_t, priority)

PERFETTO_TP_TABLE(PERFETTO_TP_SCHED_SLICE_TABLE_DEF);

// @tablegroup Events
// @param utid {@joinable thread.utid}
#define PERFETTO_TP_THREAD_STATE_TABLE_DEF(NAME, PARENT, C) \
  NAME(ThreadStateTable, "thread_state")                    \
  PERFETTO_TP_ROOT_TABLE(PARENT, C)                         \
  C(int64_t, ts)                                            \
  C(int64_t, dur)                                           \
  C(base::Optional<uint32_t>, cpu)                          \
  C(uint32_t, utid)                                         \
  C(StringPool::Id, state)                                  \
  C(base::Optional<uint32_t>, io_wait)                      \
  C(base::Optional<StringPool::Id>, blocked_function)       \
  C(base::Optional<uint32_t>, waker_utid)

PERFETTO_TP_TABLE(PERFETTO_TP_THREAD_STATE_TABLE_DEF);

// @tablegroup Events
#define PERFETTO_TP_GPU_SLICES_DEF(NAME, PARENT, C) \
  NAME(GpuSliceTable, "gpu_slice")                  \
  PARENT(PERFETTO_TP_SLICE_TABLE_DEF, C)            \
  C(base::Optional<int64_t>, context_id)            \
  C(base::Optional<int64_t>, render_target)         \
  C(StringPool::Id, render_target_name)             \
  C(base::Optional<int64_t>, render_pass)           \
  C(StringPool::Id, render_pass_name)               \
  C(base::Optional<int64_t>, command_buffer)        \
  C(StringPool::Id, command_buffer_name)            \
  C(base::Optional<uint32_t>, frame_id)             \
  C(base::Optional<uint32_t>, submission_id)        \
  C(base::Optional<int64_t>, hw_queue_id)           \
  C(StringPool::Id, render_subpasses)

PERFETTO_TP_TABLE(PERFETTO_TP_GPU_SLICES_DEF);

// @tablegroup Events
#define PERFETTO_TP_GRAPHICS_FRAME_SLICES_DEF(NAME, PARENT, C) \
  NAME(GraphicsFrameSliceTable, "frame_slice")                 \
  PARENT(PERFETTO_TP_SLICE_TABLE_DEF, C)                       \
  C(uint32_t, frame_number)                                    \
  C(StringPool::Id, layer_name)                                \
  C(int64_t, queue_to_acquire_time)                            \
  C(int64_t, acquire_to_latch_time)                            \
  C(int64_t, latch_to_present_time)

PERFETTO_TP_TABLE(PERFETTO_TP_GRAPHICS_FRAME_SLICES_DEF);

#define PERFETTO_TP_EXPECTED_FRAME_TIMELINE_SLICES_DEF(NAME, PARENT, C)  \
  NAME(ExpectedFrameTimelineSliceTable, "expected_frame_timeline_slice") \
  PARENT(PERFETTO_TP_SLICE_TABLE_DEF, C)                                 \
  C(int64_t, display_frame_token)                                        \
  C(int64_t, surface_frame_token)                                        \
  C(uint32_t, upid)                                                      \
  C(StringPool::Id, layer_name)

PERFETTO_TP_TABLE(PERFETTO_TP_EXPECTED_FRAME_TIMELINE_SLICES_DEF);

#define PERFETTO_TP_ACTUAL_FRAME_TIMELINE_SLICES_DEF(NAME, PARENT, C) \
  NAME(ActualFrameTimelineSliceTable, "actual_frame_timeline_slice")  \
  PARENT(PERFETTO_TP_SLICE_TABLE_DEF, C)                              \
  C(int64_t, display_frame_token)                                     \
  C(int64_t, surface_frame_token)                                     \
  C(uint32_t, upid)                                                   \
  C(StringPool::Id, layer_name)                                       \
  C(StringPool::Id, present_type)                                     \
  C(int32_t, on_time_finish)                                          \
  C(int32_t, gpu_composition)                                         \
  C(StringPool::Id, jank_type)                                        \
  C(StringPool::Id, prediction_type)                                  \
  C(StringPool::Id, jank_tag)

PERFETTO_TP_TABLE(PERFETTO_TP_ACTUAL_FRAME_TIMELINE_SLICES_DEF);

#define PERFETTO_TP_EXPERIMENTAL_FLAT_SLICE_TABLE_DEF(NAME, PARENT, C) \
  NAME(ExperimentalFlatSliceTable, "experimental_flat_slice")          \
  PERFETTO_TP_ROOT_TABLE(PARENT, C)                                    \
  C(int64_t, ts)                                                       \
  C(int64_t, dur)                                                      \
  C(TrackTable::Id, track_id)                                          \
  C(base::Optional<StringPool::Id>, category)                          \
  C(base::Optional<StringPool::Id>, name)                              \
  C(uint32_t, arg_set_id)                                              \
  C(base::Optional<SliceTable::Id>, source_id)                         \
  C(int64_t, start_bound, Column::Flag::kHidden)                       \
  C(int64_t, end_bound, Column::Flag::kHidden)

PERFETTO_TP_TABLE(PERFETTO_TP_EXPERIMENTAL_FLAT_SLICE_TABLE_DEF);

}  // namespace tables
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TABLES_SLICE_TABLES_H_
