/*
 * Copyright (C) 2025 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_ETW_FILE_IO_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_ETW_FILE_IO_TRACKER_H_

#include <cstdint>
#include <map>

#include "perfetto/protozero/field.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

// A class to keep track of file I/O events recorded by Event Tracing for
// Windows (ETW). File operations start and end in separate ETW events, so this
// class tracks operations that have started and records the end time when a
// corresponding "operation end" event is found. Events without a corresponding
// end event are ignored.
class FileIoTracker {
 public:
  explicit FileIoTracker(TraceProcessorContext* context);

  void ParseFileIoCreate(int64_t timestamp, protozero::ConstBytes);
  void ParseFileIoDirEnum(int64_t timestamp, protozero::ConstBytes);
  void ParseFileIoInfo(int64_t timestamp, protozero::ConstBytes);
  void ParseFileIoReadWrite(int64_t timestamp, protozero::ConstBytes);
  void ParseFileIoSimpleOp(int64_t timestamp, protozero::ConstBytes);
  void ParseFileIoOpEnd(int64_t timestamp, protozero::ConstBytes);

 private:
  struct FileIoEvent {
    int64_t timestamp;
    uint32_t opcode;
    SliceTracker::SetArgsCallback set_args;
  };

  // Starts tracking `event`, to be added to the trace when its matching end
  // event is parsed.
  void StartEvent(uint64_t irp, FileIoEvent event);

  // Adds the ending event to the trace as a slice.
  void EndEvent(int64_t end_timestamp,
                uint64_t irp,
                SliceTracker::SetArgsCallback set_args_callback);

  TraceProcessorContext* context_;

  // Keeps track of events parsed so far for which a corresponding "operation
  // end" event has not yet been parsed.
  std::map<uint64_t, FileIoEvent> started_events_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_ETW_FILE_IO_TRACKER_H_
