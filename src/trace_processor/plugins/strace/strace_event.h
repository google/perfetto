/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_PLUGINS_STRACE_STRACE_EVENT_H_
#define SRC_TRACE_PROCESSOR_PLUGINS_STRACE_STRACE_EVENT_H_

#include <cstdint>
#include <optional>

#include "src/trace_processor/containers/string_pool.h"

namespace perfetto::trace_processor::strace_importer {

// Pushed into the TraceSorter, one per strace line, in trace-timestamp
// order. Strings are interned up front (in the tokenizer) so this stays
// cheap to copy/sort.
struct alignas(8) StraceEvent {
  uint32_t tid = 0;
  StringPool::Id syscall_name_id;
  std::optional<StringPool::Id> args_id;
  std::optional<StringPool::Id> return_value_id;
  bool is_unfinished = false;
  bool is_resumed = false;
};

}  // namespace perfetto::trace_processor::strace_importer

#endif  // SRC_TRACE_PROCESSOR_PLUGINS_STRACE_STRACE_EVENT_H_
