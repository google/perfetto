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

#include "src/trace_processor/plugins/android_framework_track_event/unknown_process_logger.h"

#include <cstdint>
#include <optional>

#include "perfetto/protozero/field.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/import_logs_tracker.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"

namespace perfetto::trace_processor::android_framework_track_event {

UnknownProcessLogger::UnknownProcessLogger(TraceProcessorContext* context)
    : context_(context) {}

void UnknownProcessLogger::Record(int64_t ts,
                                  const char* source,
                                  uint32_t pid,
                                  protozero::ConstChars process_name,
                                  std::optional<int64_t> start_seq_id) {
  if (!warned_pids_.insert(pid).second) {
    return;
  }
  TraceStorage* storage = context_->storage.get();
  context_->import_logs_tracker->RecordParserLog(
      stats::android_track_event_process_unknown, ts,
      [&](ArgsInserter& inserter) {
        inserter.AddArg(storage->InternString("source"),
                        Variadic::String(storage->InternString(source)));
        inserter.AddArg(storage->InternString("pid"),
                        Variadic::UnsignedInteger(pid));
        if (process_name.size > 0) {
          inserter.AddArg(
              storage->InternString("process_name"),
              Variadic::String(storage->InternString(process_name)));
        }
        if (start_seq_id) {
          inserter.AddArg(storage->InternString("start_seq_id"),
                          Variadic::Integer(*start_seq_id));
        }
      });
}

}  // namespace perfetto::trace_processor::android_framework_track_event
