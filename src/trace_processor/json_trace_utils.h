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

#ifndef SRC_TRACE_PROCESSOR_JSON_TRACE_UTILS_H_
#define SRC_TRACE_PROCESSOR_JSON_TRACE_UTILS_H_

#include <stdint.h>

#include "perfetto/base/optional.h"

namespace Json {
class Value;
}

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;

namespace json_trace_utils {

base::Optional<int64_t> CoerceToNs(const Json::Value& value);
base::Optional<int64_t> CoerceToInt64(const Json::Value& value);
base::Optional<uint32_t> CoerceToUint32(const Json::Value& value);

}  // namespace json_trace_utils
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_JSON_TRACE_UTILS_H_
