/*
 * Copyright (C) 2022 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_PRELUDE_FUNCTIONS_PPROF_FUNCTIONS_H_
#define SRC_TRACE_PROCESSOR_PRELUDE_FUNCTIONS_PPROF_FUNCTIONS_H_

#include <sqlite3.h>

#include "perfetto/base/status.h"

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;

struct PprofFunctions {
  static base::Status Register(sqlite3* db, TraceProcessorContext* context);
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_PRELUDE_FUNCTIONS_PPROF_FUNCTIONS_H_
