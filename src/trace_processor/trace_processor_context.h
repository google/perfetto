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

#ifndef SRC_TRACE_PROCESSOR_TRACE_PROCESSOR_CONTEXT_H_
#define SRC_TRACE_PROCESSOR_TRACE_PROCESSOR_CONTEXT_H_

#include <memory>

namespace perfetto {
namespace trace_processor {

class ArgsTracker;
class ChunkedTraceReader;
class ClockTracker;
class EventTracker;
class ProcessTracker;
class SliceTracker;
class SyscallTracker;
class TraceParser;
class TraceStorage;
class TraceSorter;
class HeapProfileTracker;

class TraceProcessorContext {
 public:
  TraceProcessorContext();
  ~TraceProcessorContext();

  std::unique_ptr<ArgsTracker> args_tracker;
  std::unique_ptr<SliceTracker> slice_tracker;
  std::unique_ptr<ProcessTracker> process_tracker;
  std::unique_ptr<SyscallTracker> syscall_tracker;
  std::unique_ptr<EventTracker> event_tracker;
  std::unique_ptr<ClockTracker> clock_tracker;
  std::unique_ptr<TraceStorage> storage;
  std::unique_ptr<TraceParser> parser;
  std::unique_ptr<TraceSorter> sorter;
  std::unique_ptr<ChunkedTraceReader> chunk_reader;
  std::unique_ptr<HeapProfileTracker> heap_profile_tracker;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TRACE_PROCESSOR_CONTEXT_H_
