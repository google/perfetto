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

#ifndef SRC_TRACE_PROCESSOR_TABLES_MEMORY_TABLES_H_
#define SRC_TRACE_PROCESSOR_TABLES_MEMORY_TABLES_H_

#include "src/trace_processor/tables/macros.h"
#include "src/trace_processor/tables/track_tables.h"

namespace perfetto {
namespace trace_processor {
namespace tables {

// @tablegroup
#define PERFETTO_TP_MEMORY_SNAPSHOT_DEF(NAME, PARENT, C) \
  NAME(MemorySnapshotTable, "memory_snapshot")           \
  PERFETTO_TP_ROOT_TABLE(PARENT, C)                      \
  C(int64_t, timestamp)                                  \
  C(TrackTable::Id, track_id)                            \
  C(StringPool::Id, detail_level)

PERFETTO_TP_TABLE(PERFETTO_TP_MEMORY_SNAPSHOT_DEF);

// @tablegroup
#define PERFETTO_TP_PROCESS_MEMORY_SNAPSHOT_DEF(NAME, PARENT, C) \
  NAME(ProcessMemorySnapshotTable, "process_memory_snapshot")    \
  PERFETTO_TP_ROOT_TABLE(PARENT, C)                              \
  C(MemorySnapshotTable::Id, snapshot_id)                        \
  C(uint32_t, upid)

PERFETTO_TP_TABLE(PERFETTO_TP_PROCESS_MEMORY_SNAPSHOT_DEF);

// @tablegroup
#define PERFETTO_TP_MEMORY_SNAPSHOT_NODE_DEF(NAME, PARENT, C) \
  NAME(MemorySnapshotNodeTable, "memory_snapshot_node")       \
  PERFETTO_TP_ROOT_TABLE(PARENT, C)                           \
  C(ProcessMemorySnapshotTable::Id, process_snapshot_id)      \
  C(MemorySnapshotNodeTable::Id, parent_node_id)              \
  C(StringPool::Id, path)                                     \
  C(int64_t, size)                                            \
  C(int64_t, effective_size)                                  \
  C(base::Optional<uint32_t>, arg_set_id)

PERFETTO_TP_TABLE(PERFETTO_TP_MEMORY_SNAPSHOT_NODE_DEF);

// @tablegroup
#define PERFETTO_TP_MEMORY_SNAPSHOT_EDGE_DEF(NAME, PARENT, C) \
  NAME(MemorySnapshotEdgeTable, "memory_snapshot_edge")       \
  PERFETTO_TP_ROOT_TABLE(PARENT, C)                           \
  C(MemorySnapshotNodeTable::Id, source_node_id)              \
  C(MemorySnapshotNodeTable::Id, target_node_id)              \
  C(uint32_t, importance)

PERFETTO_TP_TABLE(PERFETTO_TP_MEMORY_SNAPSHOT_EDGE_DEF);

}  // namespace tables
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TABLES_MEMORY_TABLES_H_
