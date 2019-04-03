/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef SRC_PROFILING_MEMORY_UNWOUND_MESSAGES_H_
#define SRC_PROFILING_MEMORY_UNWOUND_MESSAGES_H_

#include <unwindstack/Maps.h>
#include <unwindstack/Unwinder.h>

#include "src/profiling/memory/wire_protocol.h"

namespace perfetto {
namespace profiling {

// A wrapper of libunwindstack FrameData that also includes the build_id.
struct FrameData {
  FrameData(unwindstack::FrameData f, std::string b)
      : frame(std::move(f)), build_id(std::move(b)) {}

  unwindstack::FrameData frame;
  std::string build_id;
};

// Single allocation with an unwound callstack.
struct AllocRecord {
  pid_t pid;
  bool error = false;
  bool reparsed_map = false;
  uint64_t unwinding_time_us = 0;
  uint64_t data_source_instance_id;
  AllocMetadata alloc_metadata;
  std::vector<FrameData> frames;
};

// Batch of deallocations.
struct FreeRecord {
  pid_t pid;
  uint64_t data_source_instance_id;
  FreeBatch free_batch;
};

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_MEMORY_UNWOUND_MESSAGES_H_
