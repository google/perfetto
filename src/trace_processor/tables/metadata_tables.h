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
#include "src/trace_processor/tables/metadata_tables_py.h"

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

// Contains information of filedescriptors collected during the trace
//
// @name filedescriptor
// @param ufd             {int64_t} Unique fd. This is != the OS fd.
//                        This is a monotonic number associated to each
//                        filedescriptor. The OS assigned fd cannot be used as
//                        primary key because fds are recycled by most kernels.
// @param fd              The OS id for this process. Note: this is *not*
//                        unique over the lifetime of the trace so cannot be
//                        used as a primary key. Use |ufd| instead.
// @param ts              The timestamp for when the fd was collected.
// @param upid            {@joinable process.upid} The upid of the process which
//                        opened the filedescriptor.
// @param path            The path to the file or device backing the fd
//                        In case this was a socket the path will be the port
//                        number.
#define PERFETTO_TP_FILEDESCRIPTOR_TABLE_DEF(NAME, PARENT, C) \
  NAME(FiledescriptorTable, "filedescriptor")                 \
  PERFETTO_TP_ROOT_TABLE(PARENT, C)                           \
  C(int64_t, fd)                                              \
  C(base::Optional<int64_t>, ts)                              \
  C(base::Optional<uint32_t>, upid)                           \
  C(base::Optional<StringPool::Id>, path)

PERFETTO_TP_TABLE(PERFETTO_TP_FILEDESCRIPTOR_TABLE_DEF);

// Experimental table, subject to arbitrary breaking changes.
#define PERFETTO_TP_EXP_MISSING_CHROME_PROC_TABLE_DEF(NAME, PARENT, C)     \
  NAME(ExpMissingChromeProcTable, "experimental_missing_chrome_processes") \
  PERFETTO_TP_ROOT_TABLE(PARENT, C)                                        \
  C(uint32_t, upid)                                                        \
  C(base::Optional<int64_t>, reliable_from)

PERFETTO_TP_TABLE(PERFETTO_TP_EXP_MISSING_CHROME_PROC_TABLE_DEF);

// Contains information of processes seen during the trace
//
// @name cpu
// @param id                     id of this CPU
// @param cluster_id             the cluster id is shared by CPUs in
//                               the same cluster
// @param processor              a string describing this core
#define PERFETTO_TP_CPU_TABLE_DEF(NAME, PARENT, C) \
  NAME(CpuTable, "cpu")                            \
  PERFETTO_TP_ROOT_TABLE(PARENT, C)                \
  C(uint32_t, cluster_id)                          \
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
