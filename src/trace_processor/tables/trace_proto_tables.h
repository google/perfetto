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

#ifndef SRC_TRACE_PROCESSOR_TABLES_TRACE_PROTO_TABLES_H_
#define SRC_TRACE_PROCESSOR_TABLES_TRACE_PROTO_TABLES_H_

#include "src/trace_processor/tables/macros.h"

namespace perfetto {
namespace trace_processor {
namespace tables {

// Experimental table, subject to arbitrary breaking changes.
#define PERFETTO_TP_EXPERIMENTAL_PROTO_CONTENT_TABLE_DEF(NAME, PARENT, C) \
  NAME(ExperimentalProtoContentTable, "experimental_proto_content")       \
  PERFETTO_TP_ROOT_TABLE(PARENT, C)                                       \
  C(StringPool::Id, path)                                                 \
  C(int64_t, total_size)

PERFETTO_TP_TABLE(PERFETTO_TP_EXPERIMENTAL_PROTO_CONTENT_TABLE_DEF);

}  // namespace tables
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TABLES_TRACE_PROTO_TABLES_H_
