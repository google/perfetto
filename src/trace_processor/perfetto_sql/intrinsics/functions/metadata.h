// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_FUNCTIONS_METADATA_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_FUNCTIONS_METADATA_H_

#include "perfetto/base/status.h"

namespace perfetto::trace_processor {

class PerfettoSqlEngine;
class TraceStorage;

// Registers the following metadata helper functions with |engine|:
//
// metadata_get_str(name STRING):
//   Returns the string value of the "primary" metadata entry for the given
//   name, prioritizing entries from the root trace and root machine.
//
// metadata_get_int(name STRING):
//   Returns the integer value of the "primary" metadata entry.
//
// metadata_get_machine_str(machine_id LONG, name STRING):
//   Returns the string value of the metadata entry for a specific machine.
//
// metadata_get_machine_int(machine_id LONG, name STRING):
//   Returns the integer value for a specific machine.
//
// metadata_get_trace_str(trace_id LONG, name STRING):
//   Returns the string value for a specific trace file.
//
// metadata_get_trace_int(trace_id LONG, name STRING):
//   Returns the integer value for a specific trace file.
base::Status RegisterMetadataFunctions(PerfettoSqlEngine& engine,
                                       TraceStorage* storage);

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_FUNCTIONS_METADATA_H_
