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

#ifndef SRC_TRACE_PROCESSOR_ARGS_TRACKER_H_
#define SRC_TRACE_PROCESSOR_ARGS_TRACKER_H_

#include "src/trace_processor/trace_processor_context.h"
#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {

// Tracks and stores args for rows until the end of the packet. This allows
// allows args to pushed as a group into storage.
class ArgsTracker {
 public:
  using Variadic = TraceStorage::Args::Variadic;

  explicit ArgsTracker(TraceProcessorContext*);

  // Adds a arg for this row id with the given key and value.
  void AddArg(RowId row_id, StringId flat_key, StringId key, Variadic);

  // Commits the added args to storage.
  void Flush();

 private:
  std::vector<TraceStorage::Args::Arg> args_;
  TraceProcessorContext* const context_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_ARGS_TRACKER_H_
