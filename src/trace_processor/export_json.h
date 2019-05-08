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

#ifndef SRC_TRACE_PROCESSOR_EXPORT_JSON_H_
#define SRC_TRACE_PROCESSOR_EXPORT_JSON_H_

#include "src/trace_processor/trace_storage.h"

#include <stdio.h>

namespace perfetto {
namespace trace_processor {
namespace json {

enum ResultCode {
  kResultOk = 0,
  kResultWrongRefType = 1,
};

// Export trace to a stream in json format.
ResultCode ExportJson(const TraceStorage* storage, FILE* output);

}  // namespace json
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_EXPORT_JSON_H_
