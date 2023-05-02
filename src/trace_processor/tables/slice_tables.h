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
#include "src/trace_processor/tables/track_tables_py.h"

namespace perfetto {
namespace trace_processor {
namespace tables {

#define PERFETTO_TP_SLICE_TABLE_DEF(NAME, PARENT, C)  \
  NAME(SliceTable, "internal_slice")                  \
  PERFETTO_TP_ROOT_TABLE(PARENT, C)                   \
  C(int64_t, ts, Column::Flag::kSorted)               \
  C(int64_t, dur)                                     \
  C(TrackTable::Id, track_id)                         \
  C(std::optional<StringPool::Id>, category)          \
  C(std::optional<StringPool::Id>, name)              \
  C(uint32_t, depth)                                  \
  C(int64_t, stack_id)                                \
  C(int64_t, parent_stack_id)                         \
  C(std::optional<SliceTable::Id>, parent_id)         \
  C(uint32_t, arg_set_id)                             \
  C(std::optional<int64_t>, thread_ts)                \
  C(std::optional<int64_t>, thread_dur)               \
  C(std::optional<int64_t>, thread_instruction_count) \
  C(std::optional<int64_t>, thread_instruction_delta)

#define PERFETTO_TP_SCHED_SLICE_TABLE_DEF(NAME, PARENT, C) \
  NAME(SchedSliceTable, "sched_slice")                     \
  PERFETTO_TP_ROOT_TABLE(PARENT, C)                        \
  C(int64_t, ts, Column::Flag::kSorted)                    \
  C(int64_t, dur)                                          \
  C(uint32_t, cpu)                                         \
  C(uint32_t, utid)                                        \
  C(StringPool::Id, end_state)                             \
  C(int32_t, priority)

}  // namespace tables
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TABLES_SLICE_TABLES_H_
