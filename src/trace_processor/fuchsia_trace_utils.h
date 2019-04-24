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

#ifndef SRC_TRACE_PROCESSOR_FUCHSIA_TRACE_UTILS_H_
#define SRC_TRACE_PROCESSOR_FUCHSIA_TRACE_UTILS_H_

#include <stddef.h>
#include <stdint.h>
#include <functional>

#include "perfetto/base/string_view.h"

namespace perfetto {
namespace trace_processor {
namespace fuchsia_trace_utils {

struct ThreadInfo {
  uint64_t pid;
  uint64_t tid;
};

template <class T>
T ReadField(uint64_t word, size_t begin, size_t end) {
  return static_cast<T>((word >> begin) &
                        ((uint64_t(1) << (end - begin + 1)) - 1));
}

bool IsInlineString(uint32_t);
base::StringView ReadInlineString(const uint64_t**, uint32_t);

bool IsInlineThread(uint32_t);
ThreadInfo ReadInlineThread(const uint64_t**);

int64_t ReadTimestamp(const uint64_t**, uint64_t);
int64_t TicksToNs(uint64_t ticks, uint64_t ticks_per_second);

}  // namespace fuchsia_trace_utils
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_FUCHSIA_TRACE_UTILS_H_
