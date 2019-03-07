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

#ifndef SRC_PROFILING_MEMORY_QUEUE_MESSAGES_H_
#define SRC_PROFILING_MEMORY_QUEUE_MESSAGES_H_

#include <unwindstack/Maps.h>
#include <unwindstack/Unwinder.h>

#include "perfetto/tracing/core/trace_writer.h"
#include "src/profiling/memory/wire_protocol.h"

// TODO(fmayer): Find better places to put these structs.

namespace perfetto {
namespace profiling {

struct UnwindingMetadata;

struct UnwindingRecord {
  pid_t pid;
  size_t size;
  std::unique_ptr<uint8_t[]> data;
  std::weak_ptr<UnwindingMetadata> metadata;
};

struct FreeRecord {
  pid_t pid;
  uint64_t data_source_instance_id;
  FreeMetadata metadata;
};

// A wrapper of libunwindstack FrameData that also includes the build_id.
struct FrameData {
  FrameData(unwindstack::FrameData f, std::string b)
      : frame(std::move(f)), build_id(std::move(b)) {}

  unwindstack::FrameData frame;
  std::string build_id;
};

struct AllocRecord {
  pid_t pid;
  uint64_t data_source_instance_id;
  AllocMetadata alloc_metadata;
  std::vector<FrameData> frames;
};

struct DumpRecord {
  std::vector<pid_t> pids;
  std::weak_ptr<TraceWriter> trace_writer;
  std::function<void()> callback;
};

struct BookkeepingRecord {
  enum class Type {
    Dump = 0,
    Malloc = 1,
    Free = 2,
  };
  pid_t pid;
  // TODO(fmayer): Use a union.
  Type record_type;
  AllocRecord alloc_record;
  FreeRecord free_record;
  DumpRecord dump_record;
};

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_MEMORY_QUEUE_MESSAGES_H_
