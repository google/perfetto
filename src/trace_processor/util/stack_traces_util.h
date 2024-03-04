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

#ifndef SRC_TRACE_PROCESSOR_UTIL_STACK_TRACES_UTIL_H_
#define SRC_TRACE_PROCESSOR_UTIL_STACK_TRACES_UTIL_H_

#include "perfetto/ext/base/string_view.h"

namespace perfetto {
namespace trace_processor {
namespace util {

// Returns whether this string is of a hex chrome module or not to decide
// whether to convert the module to/from hex.
// TODO(b/148109467): Remove workaround once all active Chrome versions
// write raw bytes instead of a string as build_id.
bool IsHexModuleId(base::StringView module);

}  // namespace util
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_UTIL_STACK_TRACES_UTIL_H_
