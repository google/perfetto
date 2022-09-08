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

#ifndef SRC_TRACE_PROCESSOR_TABLES_TRACK_TABLES_H_
#define SRC_TRACE_PROCESSOR_TABLES_TRACK_TABLES_H_

#include "src/trace_processor/tables/macros.h"

namespace perfetto {
namespace trace_processor {
namespace tables {

// @tablegroup Tracks
// @param source_arg_set_id {@joinable args.arg_set_id}
// @param parent_id id of a parent track {@joinable track.id}
#define PERFETTO_TP_TRACK_TABLE_DEF(NAME, PARENT, C) \
  NAME(TrackTable, "track")                          \
  PERFETTO_TP_ROOT_TABLE(PARENT, C)                  \
  C(StringPool::Id, name)                            \
  C(base::Optional<TrackTable::Id>, parent_id)       \
  C(base::Optional<uint32_t>, source_arg_set_id)

PERFETTO_TP_TABLE(PERFETTO_TP_TRACK_TABLE_DEF);

// @tablegroup Tracks
#define PERFETTO_TP_PROCESS_TRACK_TABLE_DEF(NAME, PARENT, C) \
  NAME(ProcessTrackTable, "process_track")                   \
  PARENT(PERFETTO_TP_TRACK_TABLE_DEF, C)                     \
  C(uint32_t, upid)

PERFETTO_TP_TABLE(PERFETTO_TP_PROCESS_TRACK_TABLE_DEF);

// @tablegroup Tracks
#define PERFETTO_TP_THREAD_TRACK_TABLE_DEF(NAME, PARENT, C) \
  NAME(ThreadTrackTable, "thread_track")                    \
  PARENT(PERFETTO_TP_TRACK_TABLE_DEF, C)                    \
  C(uint32_t, utid)

PERFETTO_TP_TABLE(PERFETTO_TP_THREAD_TRACK_TABLE_DEF);

// @tablegroup Tracks
#define PERFETTO_TP_GPU_TRACK_DEF(NAME, PARENT, C) \
  NAME(GpuTrackTable, "gpu_track")                 \
  PARENT(PERFETTO_TP_TRACK_TABLE_DEF, C)           \
  C(StringPool::Id, scope)                         \
  C(StringPool::Id, description)                   \
  C(base::Optional<int64_t>, context_id)

PERFETTO_TP_TABLE(PERFETTO_TP_GPU_TRACK_DEF);

// @tablegroup Tracks
#define PERFETTO_TP_COUNTER_TRACK_DEF(NAME, PARENT, C) \
  NAME(CounterTrackTable, "counter_track")             \
  PARENT(PERFETTO_TP_TRACK_TABLE_DEF, C)               \
  C(StringPool::Id, unit)                              \
  C(StringPool::Id, description)

PERFETTO_TP_TABLE(PERFETTO_TP_COUNTER_TRACK_DEF);

// @tablegroup Tracks
#define PERFETTO_TP_THREAD_COUNTER_TRACK_DEF(NAME, PARENT, C) \
  NAME(ThreadCounterTrackTable, "thread_counter_track")       \
  PARENT(PERFETTO_TP_COUNTER_TRACK_DEF, C)                    \
  C(uint32_t, utid)

PERFETTO_TP_TABLE(PERFETTO_TP_THREAD_COUNTER_TRACK_DEF);

// @tablegroup Tracks
#define PERFETTO_TP_PROCESS_COUNTER_TRACK_DEF(NAME, PARENT, C) \
  NAME(ProcessCounterTrackTable, "process_counter_track")      \
  PARENT(PERFETTO_TP_COUNTER_TRACK_DEF, C)                     \
  C(uint32_t, upid)

PERFETTO_TP_TABLE(PERFETTO_TP_PROCESS_COUNTER_TRACK_DEF);

// @tablegroup Tracks
#define PERFETTO_TP_CPU_COUNTER_TRACK_DEF(NAME, PARENT, C) \
  NAME(CpuCounterTrackTable, "cpu_counter_track")          \
  PARENT(PERFETTO_TP_COUNTER_TRACK_DEF, C)                 \
  C(uint32_t, cpu)

PERFETTO_TP_TABLE(PERFETTO_TP_CPU_COUNTER_TRACK_DEF);

// @tablegroup Tracks
#define PERFETTO_TP_IRQ_COUNTER_TRACK_DEF(NAME, PARENT, C) \
  NAME(IrqCounterTrackTable, "irq_counter_track")          \
  PARENT(PERFETTO_TP_COUNTER_TRACK_DEF, C)                 \
  C(int32_t, irq)

PERFETTO_TP_TABLE(PERFETTO_TP_IRQ_COUNTER_TRACK_DEF);

// @tablegroup Tracks
#define PERFETTO_TP_SOFTIRQ_COUNTER_TRACK_DEF(NAME, PARENT, C) \
  NAME(SoftirqCounterTrackTable, "softirq_counter_track")      \
  PARENT(PERFETTO_TP_COUNTER_TRACK_DEF, C)                     \
  C(int32_t, softirq)

PERFETTO_TP_TABLE(PERFETTO_TP_SOFTIRQ_COUNTER_TRACK_DEF);

// @tablegroup Tracks
#define PERFETTO_TP_GPU_COUNTER_TRACK_DEF(NAME, PARENT, C) \
  NAME(GpuCounterTrackTable, "gpu_counter_track")          \
  PARENT(PERFETTO_TP_COUNTER_TRACK_DEF, C)                 \
  C(uint32_t, gpu_id)

PERFETTO_TP_TABLE(PERFETTO_TP_GPU_COUNTER_TRACK_DEF);

// Sampled counters' values for samples in the perf_sample table.
//
// @param perf_session_id id of a distict profiling stream.
//        {@joinable perf_sample.perf_session_id}
// @param cpu the core the sample was taken on.
// @param is_timebase if true, this counter was the sampling
//        timebase for this perf_session_id.
// @tablegroup Tracks
#define PERFETTO_TP_PERF_COUNTER_TRACK_DEF(NAME, PARENT, C) \
  NAME(PerfCounterTrackTable, "perf_counter_track")         \
  PARENT(PERFETTO_TP_COUNTER_TRACK_DEF, C)                  \
  C(uint32_t, perf_session_id)                              \
  C(uint32_t, cpu)                                          \
  C(uint32_t, is_timebase)

PERFETTO_TP_TABLE(PERFETTO_TP_PERF_COUNTER_TRACK_DEF);

// Energy consumers' values for energy descriptors in
// energy_estimation_breakdown packet
//
// @param consumer_id id of a distinct energy consumer
// @param consumer_type type of energy consumer
// @param ordinal ordinal of energy consumer
// @tablegroup Tracks
#define PERFETTO_TP_ENERGY_COUNTER_TRACK_DEF(NAME, PARENT, C) \
  NAME(EnergyCounterTrackTable, "energy_counter_track")       \
  PARENT(PERFETTO_TP_COUNTER_TRACK_DEF, C)                    \
  C(int32_t, consumer_id)                                     \
  C(StringPool::Id, consumer_type)                            \
  C(int32_t, ordinal)

PERFETTO_TP_TABLE(PERFETTO_TP_ENERGY_COUNTER_TRACK_DEF);

// Energy per process values for per_uid in energy_estimation_breakdown packet
//
// @param uid id of distinct energy process
// @tablegroup Tracks
#define PERFETTO_TP_UID_COUNTER_TRACK_DEF(NAME, PARENT, C) \
  NAME(UidCounterTrackTable, "uid_counter_track")          \
  PARENT(PERFETTO_TP_COUNTER_TRACK_DEF, C)                 \
  C(int32_t, uid)

PERFETTO_TP_TABLE(PERFETTO_TP_UID_COUNTER_TRACK_DEF);

// Energy consumer values for per uid in uid_counter_track
//
// @param consumer_id of consumer of process
// @tablegroup Tracks
#define PERFETTO_TP_ENERGY_PER_UID_COUNTER_TRACK_DEF(NAME, PARENT, C) \
  NAME(EnergyPerUidCounterTrackTable, "energy_per_uid_counter_track") \
  PARENT(PERFETTO_TP_UID_COUNTER_TRACK_DEF, C)                        \
  C(int32_t, consumer_id)

PERFETTO_TP_TABLE(PERFETTO_TP_ENERGY_PER_UID_COUNTER_TRACK_DEF);

}  // namespace tables
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TABLES_TRACK_TABLES_H_
