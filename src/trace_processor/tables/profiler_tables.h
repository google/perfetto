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

#ifndef SRC_TRACE_PROCESSOR_TABLES_PROFILER_TABLES_H_
#define SRC_TRACE_PROCESSOR_TABLES_PROFILER_TABLES_H_

#include "src/trace_processor/tables/macros.h"

namespace perfetto {
namespace trace_processor {
namespace tables {

#define PERFETTO_TP_SYMBOL_DEF(NAME, PARENT, C) \
  NAME(SymbolTable, "stack_profile_symbol")     \
  PERFETTO_TP_ROOT_TABLE(PARENT, C)             \
  C(uint32_t, symbol_set_id)                    \
  C(StringPool::Id, name)                       \
  C(StringPool::Id, source_file)                \
  C(uint32_t, line_number)

PERFETTO_TP_TABLE(PERFETTO_TP_SYMBOL_DEF);

#define PERFETTO_TP_HEAP_GRAPH_OBJECT_DEF(NAME, PARENT, C) \
  NAME(HeapGraphObjectTable, "heap_graph_object")          \
  PERFETTO_TP_ROOT_TABLE(PARENT, C)                        \
  C(int64_t, upid)                                         \
  C(int64_t, graph_sample_ts)                              \
  C(int64_t, object_id)                                    \
  C(int64_t, self_size)                                    \
  C(StringPool::Id, type_name)

PERFETTO_TP_TABLE(PERFETTO_TP_HEAP_GRAPH_OBJECT_DEF);

}  // namespace tables
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TABLES_PROFILER_TABLES_H_
