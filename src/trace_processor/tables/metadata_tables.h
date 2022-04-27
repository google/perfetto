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

#ifndef SRC_TRACE_PROCESSOR_TABLES_METADATA_TABLES_H_
#define SRC_TRACE_PROCESSOR_TABLES_METADATA_TABLES_H_

#include "src/trace_processor/tables/macros.h"

namespace perfetto {
namespace trace_processor {
namespace tables {

// @param arg_set_id {@joinable args.arg_set_id}
#define PERFETTO_TP_RAW_TABLE_DEF(NAME, PARENT, C) \
  NAME(RawTable, "raw")                            \
  PERFETTO_TP_ROOT_TABLE(PARENT, C)                \
  C(int64_t, ts, Column::Flag::kSorted)            \
  C(StringPool::Id, name)                          \
  C(uint32_t, cpu)                                 \
  C(uint32_t, utid)                                \
  C(uint32_t, arg_set_id)

PERFETTO_TP_TABLE(PERFETTO_TP_RAW_TABLE_DEF);

// @name args
#define PERFETTO_TP_ARG_TABLE_DEF(NAME, PARENT, C) \
  NAME(ArgTable, "internal_args")                  \
  PERFETTO_TP_ROOT_TABLE(PARENT, C)                \
  C(uint32_t, arg_set_id, Column::Flag::kSorted)   \
  C(StringPool::Id, flat_key)                      \
  C(StringPool::Id, key)                           \
  C(base::Optional<int64_t>, int_value)            \
  C(base::Optional<StringPool::Id>, string_value)  \
  C(base::Optional<double>, real_value)            \
  C(StringPool::Id, value_type)

PERFETTO_TP_TABLE(PERFETTO_TP_ARG_TABLE_DEF);

#define PERFETTO_TP_METADATA_TABLE_DEF(NAME, PARENT, C) \
  NAME(MetadataTable, "metadata")                       \
  PERFETTO_TP_ROOT_TABLE(PARENT, C)                     \
  C(StringPool::Id, name)                               \
  C(StringPool::Id, key_type)                           \
  C(base::Optional<int64_t>, int_value)                 \
  C(base::Optional<StringPool::Id>, str_value)

PERFETTO_TP_TABLE(PERFETTO_TP_METADATA_TABLE_DEF);

// Contains information of threads seen during the trace
//
// @name thread
// @param utid             {uint32_t} Unique thread id. This is != the OS tid.
//                         This is a monotonic number associated to each thread.
//                         The OS thread id (tid) cannot be used as primary key
//                         because tids and pids are recycled by most kernels.
// @param tid              The OS id for this thread. Note: this is *not*
//                         unique over the lifetime of the trace so cannot be
//                         used as a primary key. Use |utid| instead.
// @param name             The name of the thread. Can be populated from many
//                         sources (e.g. ftrace, /proc scraping, track event
//                         etc).
// @param start_ts         The start timestamp of this thread (if known). Is
//                         null in most cases unless a thread creation event is
//                         enabled (e.g. task_newtask ftrace event on
//                         Linux/Android).
// @param end_ts           The end timestamp of this thread (if known). Is
//                         null in most cases unless a thread destruction event
//                         is enabled (e.g. sched_process_free ftrace event on
//                         Linux/Android).
// @param upid             {@joinable process.upid} The process hosting this
//                         thread.
// @param is_main_thread   Boolean indicating if this thread is the main thread
//                         in the process.
#define PERFETTO_TP_THREAD_TABLE_DEF(NAME, PARENT, C) \
  NAME(ThreadTable, "internal_thread")                \
  PERFETTO_TP_ROOT_TABLE(PARENT, C)                   \
  C(uint32_t, tid)                                    \
  C(base::Optional<StringPool::Id>, name)             \
  C(base::Optional<int64_t>, start_ts)                \
  C(base::Optional<int64_t>, end_ts)                  \
  C(base::Optional<uint32_t>, upid)                   \
  C(base::Optional<uint32_t>, is_main_thread)

PERFETTO_TP_TABLE(PERFETTO_TP_THREAD_TABLE_DEF);

// Contains information of processes seen during the trace
//
// @name process
// @param upid            {uint32_t} Unique process id. This is != the OS pid.
//                        This is a monotonic number associated to each process.
//                        The OS process id (pid) cannot be used as primary key
//                        because tids and pids are recycled by most kernels.
// @param pid             The OS id for this process. Note: this is *not*
//                        unique over the lifetime of the trace so cannot be
//                        used as a primary key. Use |upid| instead.
// @param name            The name of the process. Can be populated from many
//                        sources (e.g. ftrace, /proc scraping, track event
//                        etc).
// @param start_ts        The start timestamp of this process (if known). Is
//                        null in most cases unless a process creation event is
//                        enabled (e.g. task_newtask ftrace event on
//                        Linux/Android).
// @param end_ts          The end timestamp of this process (if known). Is
//                        null in most cases unless a process destruction event
//                        is enabled (e.g. sched_process_free ftrace event on
//                        Linux/Android).
// @param parent_upid     {@joinable process.upid} The upid of the process which
//                        caused this process to be spawned.
// @param uid             {@joinable package_list.uid} The Unix user id of the
//                        process.
// @param android_appid   Android appid of this process.
// @param cmdline         /proc/cmdline for this process.
// @param arg_set_id      {@joinable args.arg_set_id} Extra args for this
//                        process.
#define PERFETTO_TP_PROCESS_TABLE_DEF(NAME, PARENT, C) \
  NAME(ProcessTable, "internal_process")               \
  PERFETTO_TP_ROOT_TABLE(PARENT, C)                    \
  C(uint32_t, pid)                                     \
  C(base::Optional<StringPool::Id>, name)              \
  C(base::Optional<int64_t>, start_ts)                 \
  C(base::Optional<int64_t>, end_ts)                   \
  C(base::Optional<uint32_t>, parent_upid)             \
  C(base::Optional<uint32_t>, uid)                     \
  C(base::Optional<uint32_t>, android_appid)           \
  C(base::Optional<StringPool::Id>, cmdline)           \
  C(uint32_t, arg_set_id)

PERFETTO_TP_TABLE(PERFETTO_TP_PROCESS_TABLE_DEF);

// Contains information of processes seen during the trace
//
// @name cpu
// @param id                     id of this CPU
// @param cluster_id             the cluster id is shared by CPUs in
//                               the same cluster
// @param time_in_state_cpu_id   a deprecated alias for cluster_id
// @param processor              a string describing this core
#define PERFETTO_TP_CPU_TABLE_DEF(NAME, PARENT, C) \
  NAME(CpuTable, "cpu")                            \
  PERFETTO_TP_ROOT_TABLE(PARENT, C)                \
  C(uint32_t, cluster_id)                          \
  C(uint32_t, time_in_state_cpu_id)                \
  C(StringPool::Id, processor)

PERFETTO_TP_TABLE(PERFETTO_TP_CPU_TABLE_DEF);

#define PERFETTO_TP_CPU_FREQ_TABLE_DEF(NAME, PARENT, C) \
  NAME(CpuFreqTable, "cpu_freq")                        \
  PERFETTO_TP_ROOT_TABLE(PARENT, C)                     \
  C(CpuTable::Id, cpu_id)                               \
  C(uint32_t, freq)

PERFETTO_TP_TABLE(PERFETTO_TP_CPU_FREQ_TABLE_DEF);

// Contains all the mapping between clock snapshots and trace time.
//
// NOTE: this table is not sorted by timestamp; this is why we omit the
// sorted flag on the ts column.
//
// @param ts            timestamp of the snapshot in trace time.
// @param clock_id      id of the clock (corresponds to the id in the trace).
// @param clock_name    the name of the clock for builtin clocks or null
//                      otherwise.
// @param clock_value   timestamp of the snapshot in clock time.
// @param snapshot_id   the index of this snapshot (only useful for debugging)
#define PERFETTO_TP_CLOCK_SNAPSHOT_TABLE_DEF(NAME, PARENT, C) \
  NAME(ClockSnapshotTable, "clock_snapshot")                  \
  PERFETTO_TP_ROOT_TABLE(PARENT, C)                           \
  C(int64_t, ts)                                              \
  C(int64_t, clock_id)                                        \
  C(base::Optional<StringPool::Id>, clock_name)               \
  C(int64_t, clock_value)                                     \
  C(uint32_t, snapshot_id)

PERFETTO_TP_TABLE(PERFETTO_TP_CLOCK_SNAPSHOT_TABLE_DEF);

}  // namespace tables
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TABLES_METADATA_TABLES_H_
